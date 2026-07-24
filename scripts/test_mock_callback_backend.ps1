[CmdletBinding()]
param(
    [string]$Root = "",
    [int]$Port = 0
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) {
    (Resolve-Path $Root).Path
}
else {
    (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$Node = (Get-Command node.exe -ErrorAction Stop).Source
if ($Port -le 0) {
    $listener = [Net.Sockets.TcpListener]::new(
        [Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $Port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
}
$secret = [Guid]::NewGuid().ToString("N")
$controlToken = [Guid]::NewGuid().ToString("N")
$eventId = "evt_mock_" + [Guid]::NewGuid().ToString("N")
$process = $null

function Invoke-StatusRequest {
    param(
        [string]$Uri,
        [string]$Method,
        [hashtable]$Headers,
        [string]$Body = ""
    )
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $Uri `
            -Method $Method -Headers $Headers -Body $Body `
            -ContentType "application/json" -TimeoutSec 5
        return @{
            Status = [int]$response.StatusCode
            Body = $response.Content
        }
    }
    catch {
        if ($_.Exception.Response) {
            $status = [int]$_.Exception.Response.StatusCode
            $reader = [IO.StreamReader]::new(
                $_.Exception.Response.GetResponseStream())
            try {
                return @{ Status = $status; Body = $reader.ReadToEnd() }
            }
            finally {
                $reader.Dispose()
            }
        }
        throw
    }
}

function New-CallbackHeaders {
    param(
        [string]$Timestamp,
        [string]$Payload,
        [switch]$Invalid
    )
    $hmac = [Security.Cryptography.HMACSHA256]::new(
        [Text.Encoding]::UTF8.GetBytes($secret))
    try {
        $signatureBytes = $hmac.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes(
                $Timestamp + "`n" + $Payload))
        $signature = ([BitConverter]::ToString($signatureBytes)).
            Replace("-", "").ToLowerInvariant()
    }
    finally {
        $hmac.Dispose()
    }
    if ($Invalid) {
        $signature = "0" * 64
    }
    return @{
        "Idempotency-Key" = $eventId
        "X-Event-Id" = $eventId
        "X-Timestamp" = $Timestamp
        "X-Signature-Version" = "1"
        "X-Signature" = $signature
    }
}

try {
    $env:YOLO11_MOCK_CALLBACK_PORT = [string]$Port
    $env:YOLO11_MOCK_CALLBACK_SECRET = $secret
    $env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN = $controlToken
    $env:YOLO11_MOCK_CALLBACK_FAIL_FIRST = "1"
    $script = Join-Path $ProjectRoot "scripts\mock_callback_backend.js"
    $process = Start-Process -FilePath $Node -ArgumentList @("`"$script`"") `
        -WorkingDirectory $ProjectRoot -PassThru -WindowStyle Hidden

    $baseUrl = "http://127.0.0.1:$Port"
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            $health = Invoke-RestMethod -Uri "$baseUrl/health" -TimeoutSec 1
            if ($health.success) { $ready = $true; break }
        }
        catch {
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw "mock callback backend did not become ready" }

    $payload = @{
        schema_version = "1.0"
        event_id = $eventId
        event_kind = "algorithm_alert"
    } | ConvertTo-Json -Compress
    $timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
    $headers = New-CallbackHeaders -Timestamp $timestamp -Payload $payload
    $first = Invoke-StatusRequest -Uri "$baseUrl/api/v1/algorithm-alerts" `
        -Method POST -Headers $headers -Body $payload
    if ($first.Status -ne 503) {
        throw "first callback should be retryable 503, got $($first.Status)"
    }
    $second = Invoke-StatusRequest -Uri "$baseUrl/api/v1/algorithm-alerts" `
        -Method POST -Headers $headers -Body $payload
    if ($second.Status -ne 204) {
        throw "second callback should be accepted with 204, got $($second.Status)"
    }
    $duplicate = Invoke-StatusRequest -Uri "$baseUrl/api/v1/algorithm-alerts" `
        -Method POST -Headers $headers -Body $payload
    if ($duplicate.Status -ne 200) {
        throw "duplicate callback should be idempotent 200, got $($duplicate.Status)"
    }
    $invalidHeaders = New-CallbackHeaders -Timestamp $timestamp `
        -Payload $payload -Invalid
    $invalid = Invoke-StatusRequest -Uri "$baseUrl/api/v1/algorithm-alerts" `
        -Method POST -Headers $invalidHeaders -Body $payload
    if ($invalid.Status -ne 401) {
        throw "invalid signature should be rejected with 401, got $($invalid.Status)"
    }
    $events = Invoke-RestMethod -Uri "$baseUrl/api/test/events" `
        -Headers @{ Authorization = "Bearer $controlToken" } -TimeoutSec 5
    if (-not $events.success -or $events.items.Count -ne 1 -or
        $events.items[0].event_id -ne $eventId) {
        throw "mock backend must store exactly one deduplicated event"
    }
    $reset = Invoke-RestMethod -Method Post `
        -Uri "$baseUrl/api/test/reset" `
        -Headers @{ Authorization = "Bearer $controlToken" } `
        -ContentType "application/json" `
        -Body '{"fail_first":2}' -TimeoutSec 5
    if (-not $reset.success -or $reset.fail_first -ne 2) {
        throw "mock backend must accept authenticated failure reconfiguration"
    }
    $health = Invoke-RestMethod -Uri "$baseUrl/health" -TimeoutSec 5
    if ($health.fail_first -ne 2 -or $health.accepted_events -ne 0) {
        throw "mock backend reset must clear events and expose failure mode"
    }
    Write-Host "PASS: mock callback verifies retry, HMAC, idempotency, and controlled failure injection." `
        -ForegroundColor Green
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    Remove-Item Env:YOLO11_MOCK_CALLBACK_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:YOLO11_MOCK_CALLBACK_SECRET -ErrorAction SilentlyContinue
    Remove-Item Env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:YOLO11_MOCK_CALLBACK_FAIL_FIRST -ErrorAction SilentlyContinue
}
