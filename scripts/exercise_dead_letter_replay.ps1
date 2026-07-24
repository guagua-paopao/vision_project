[CmdletBinding()]
param(
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
    [string]$MockCallbackBase = "http://127.0.0.1:9095",
    [string]$CameraProfile = "entry_camera_01",
    [string]$CallbackProfile = "backend_primary",
    [int]$WaitSeconds = 180,
    [string]$EvidenceDir = ""
)

$ErrorActionPreference = "Stop"
$adminToken = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
$mockControlToken = $env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN
if ([string]::IsNullOrWhiteSpace($adminToken) -or
    [string]::IsNullOrWhiteSpace($mockControlToken)) {
    throw "Admin and mock control tokens are required in the process environment."
}
$ApiBase = $ApiBase.TrimEnd("/")
$MockCallbackBase = $MockCallbackBase.TrimEnd("/")
$cameraId = "p6_dead_" +
    [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
$headers = @{ Authorization = "Bearer $adminToken" }
$mockHeaders = @{ Authorization = "Bearer $mockControlToken" }
$created = $false

function Invoke-Api {
    param(
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [hashtable]$ExtraHeaders = @{}
    )
    $requestHeaders = @{}
    foreach ($item in $headers.GetEnumerator()) {
        $requestHeaders[$item.Key] = $item.Value
    }
    foreach ($item in $ExtraHeaders.GetEnumerator()) {
        $requestHeaders[$item.Key] = $item.Value
    }
    $parameters = @{
        Uri = "$ApiBase$Path"
        Method = $Method
        Headers = $requestHeaders
        UseBasicParsing = $true
        TimeoutSec = 10
    }
    if ($null -ne $Body) {
        $parameters.ContentType = "application/json"
        $parameters.Body = if ($Body -is [string]) {
            $Body
        }
        else {
            $Body | ConvertTo-Json -Depth 12 -Compress
        }
    }
    return Invoke-WebRequest @parameters
}

function Read-Json([object]$Response) {
    if (-not $Response.Content) { return $null }
    return $Response.Content | ConvertFrom-Json
}

function Reset-Mock([int]$FailFirst) {
    $result = Invoke-RestMethod -Method Post `
        -Uri "$MockCallbackBase/api/test/reset" `
        -Headers $mockHeaders `
        -ContentType "application/json" `
        -Body (@{ fail_first = $FailFirst } | ConvertTo-Json -Compress) `
        -TimeoutSec 5
    if (-not $result.success -or $result.fail_first -ne $FailFirst) {
        throw "Mock callback failure mode did not update."
    }
}

try {
    if (-not $EvidenceDir) {
        $stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
        $EvidenceDir = Join-Path (
            Resolve-Path (Join-Path $PSScriptRoot "..")
        ) "reports\p6\dead_letter\$stamp"
    }
    New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

    # Keep every callback failing until one event exhausts the two-attempt
    # runtime budget. A small fail-first value is nondeterministic when several
    # alerts are queued concurrently because different events can consume it.
    Reset-Mock 100
    $body = @{
        camera_id = $cameraId
        name = "P6 dead-letter replay exercise"
        camera_profile = $CameraProfile
        desired_state = "stopped"
        frame_interval_ms = 500
        output_mode = "latest"
        jpeg_quality = 85
        max_width = 1280
        max_height = 720
        retention_days = 1
        max_saved_frames = 100
        analysis = @{
            enabled = $true
            target_infer_fps = 5.0
            algorithm_profile = "security_default"
            algorithms = @(
                "people_flow",
                "electronic_fence",
                "pose_action"
            )
        }
        callback_profile = $CallbackProfile
    }
    $create = Invoke-Api -Method POST -Path "/cameras" -Body $body `
        -ExtraHeaders @{
            "Idempotency-Key" = "p6-dead-create-$cameraId"
        }
    if ($create.StatusCode -ne 201) {
        throw "Dead-letter exercise camera create failed."
    }
    $created = $true
    $start = Invoke-Api -Method POST -Path "/cameras/$cameraId/start" `
        -ExtraHeaders @{
            "Idempotency-Key" = "p6-dead-start-$cameraId"
        }
    if ($start.StatusCode -notin @(200, 202)) {
        throw "Dead-letter exercise camera start failed."
    }

    $dead = $null
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        $response = Read-Json (
            Invoke-Api -Method GET -Path (
                "/operations/callbacks?status=dead_letter&camera_id=" +
                "$cameraId&limit=20&offset=0"
            )
        )
        if ($response.items.Count -gt 0) {
            $dead = $response.items[0]
            break
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $dead) {
        throw "No real dead-letter was produced for $cameraId."
    }
    if ([int]$dead.attempt -ne 2) {
        throw "Dead-letter did not consume the expected bounded attempt budget."
    }

    Reset-Mock 0
    $replay = Read-Json (
        Invoke-Api -Method POST `
            -Path "/operations/callbacks/$($dead.outbox_id)/replay" `
            -Body "{}" `
            -ExtraHeaders @{
                "If-Match" = '"' + [string]$dead.attempt + '"'
            }
    )
    if ($replay.delivery.status -ne "retry") {
        throw "Dead-letter replay was not queued."
    }

    $delivered = $false
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $events = Invoke-RestMethod `
            -Uri "$MockCallbackBase/api/test/events" `
            -Headers $mockHeaders -TimeoutSec 5
        foreach ($event in @($events.items)) {
            if ($event.payload.camera_id -eq $cameraId) {
                $delivered = $true
                break
            }
        }
        if ($delivered) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $delivered) {
        throw "Replayed dead-letter was not delivered to the callback backend."
    }

    $delivery = $null
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $response = Read-Json (
            Invoke-Api -Method GET -Path (
                "/operations/callbacks?status=delivered&camera_id=" +
                "$cameraId&limit=20&offset=0"
            )
        )
        $delivery = @($response.items |
            Where-Object { $_.outbox_id -eq $dead.outbox_id }) |
            Select-Object -First 1
        if ($delivery) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $delivery) {
        throw "Replayed callback was received but did not reach durable delivered."
    }

    [ordered]@{
        passed = $true
        camera_id = $cameraId
        outbox_id = $dead.outbox_id
        event_id = $dead.event_id
        dead_letter_attempt = [int]$dead.attempt
        replay_status = $replay.delivery.status
        final_status = $delivery.status
        receiver_observed = $true
    } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (
            Join-Path $EvidenceDir "summary.json") -Encoding UTF8

    Write-Host (
        "PASS: real callback reached dead-letter after 2 attempts and was " +
        "manually replayed to delivered."
    ) -ForegroundColor Green
}
finally {
    try {
        Reset-Mock 1
    }
    catch {
    }
    if ($created) {
        try {
            Invoke-Api -Method POST -Path "/cameras/$cameraId/stop" |
                Out-Null
        }
        catch {
        }
        try {
            $current = Read-Json (
                Invoke-Api -Method GET -Path "/cameras/$cameraId")
            Invoke-Api -Method DELETE -Path "/cameras/$cameraId" `
                -ExtraHeaders @{
                    "If-Match" = '"' +
                        [string]$current.camera.version + '"'
                } | Out-Null
        }
        catch {
            Write-Warning "Dead-letter exercise camera cleanup needs review."
        }
    }
    $adminToken = $null
    $mockControlToken = $null
}
