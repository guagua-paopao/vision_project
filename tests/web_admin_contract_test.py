from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
INDEX = (ROOT / "web/camera-admin/index.html").read_text(encoding="utf-8")
SCRIPT = (ROOT / "web/camera-admin/app.js").read_text(encoding="utf-8")
STYLE = (ROOT / "web/camera-admin/styles.css").read_text(encoding="utf-8")
SERVER = (ROOT / "src/server/people_flow_http_server.cpp").read_text(
    encoding="utf-8"
)
CONTROLLER = (
    ROOT / "src/server/camera_task_http_controller.cpp"
).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    required_ids = {
        "camera-rows",
        "monitor-camera",
        "monitor-kpis",
        "phase-cards",
        "analysis-snapshot",
        "alert-camera",
        "alert-rows",
        "analysis-enabled",
        "analysis-fps",
        "algorithm-profile",
    }
    present_ids = set(re.findall(r'\bid="([^"]+)"', INDEX))
    require(
        required_ids <= present_ids,
        f"Web parity elements missing: {sorted(required_ids - present_ids)}",
    )

    require(
        "/people-flow/" not in SCRIPT,
        "Camera Admin JavaScript must not call /people-flow/*",
    )
    for fragment in (
        "/cameras/${encodeURIComponent(cameraId)}/status",
        "/cameras/${encodeURIComponent(cameraId)}/analysis-snapshot",
        "/cameras/${encodeURIComponent(cameraId)}/alerts?",
        "initial_occupancy",
        "in_count",
        "out_count",
        "occupancy",
        "live_persons",
        "'phase1', 'phase2', 'phase3', 'phase4'",
    ):
        require(fragment in SCRIPT, f"Web parity contract missing: {fragment}")

    for forbidden in ("localStorage", "sessionStorage", "document.cookie"):
        require(
            forbidden not in SCRIPT,
            f"Admin Token must not use persistent browser storage: {forbidden}",
        )
    require(
        "escapeHtml" in SCRIPT and "textContent" in SCRIPT,
        "dynamic Web output must use escaping or textContent",
    )
    require(
        "Authorization: `Bearer ${state.token}`" in SCRIPT,
        "Camera Admin API calls must retain Bearer authentication",
    )

    require(
        '"/api/v1/cameras/<string>/analysis-snapshot"' in CONTROLLER,
        "Camera API must expose the annotated analysis snapshot",
    )
    require(
        "resolveCameraArtifact" in CONTROLLER
        and "pathWithin(root, resolved)" in CONTROLLER
        and "isReparsePoint" in CONTROLLER,
        "analysis snapshot reads must remain inside the configured output root",
    )

    for directive in (
        "base-uri 'none'",
        "object-src 'none'",
        "frame-ancestors 'none'",
        "script-src 'self'",
        "connect-src 'self'",
        "Referrer-Policy",
        "Permissions-Policy",
    ):
        require(directive in SERVER, f"Camera Admin security header missing: {directive}")

    require(
        ".monitor-layout" in STYLE
        and ".phase-grid" in STYLE
        and "@media (max-width: 720px)" in STYLE,
        "Camera Admin must include desktop and responsive monitoring layouts",
    )

    print("PASS: Camera Admin R6 API, parity, token, CSP, and DOM contracts")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
