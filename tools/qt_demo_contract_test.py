#!/usr/bin/env python3
"""Contract smoke test for the Qt client demo API and endpoint wiring."""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MOCK_SERVER = ROOT / "tools" / "mock_people_flow_server.py"
API_CLIENT_SOURCE = ROOT / "qt_client" / "src" / "people_flow_api_client.cpp"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(base: str, method: str, path: str, payload: dict | None = None) -> tuple[int, bytes, str]:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(base + path, data=body, method=method)
    req.add_header("Accept", "application/json")
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            return response.status, response.read(), response.headers.get_content_type()
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers.get_content_type()


def json_request(base: str, method: str, path: str, payload: dict | None = None) -> tuple[int, dict]:
    status, body, content_type = request(base, method, path, payload)
    assert content_type == "application/json", (status, content_type, body[:200])
    return status, json.loads(body)


def wait_ready(base: str) -> None:
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            status, payload = json_request(base, "GET", "/api/v1/health")
            if status == 200 and payload.get("success"):
                return
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    raise AssertionError("mock server did not become ready")


def verify_qt_endpoint_wiring() -> None:
    source = API_CLIENT_SOURCE.read_text(encoding="utf-8")
    required_fragments = [
        "/api/v1/health",
        "/api/v1/people-flow/start",
        "/api/v1/people-flow/%1/stop",
        "/api/v1/people-flow/%1/status",
        "/api/v1/people-flow/%1/snapshot",
        "/api/v1/people-flow/%1/security",
        "/api/v1/people-flow/cameras/%1/realtime",
        "/api/v1/people-flow/cameras/%1/events",
    ]
    missing = [fragment for fragment in required_fragments if fragment not in source]
    assert not missing, f"Qt API client is missing endpoint wiring: {missing}"
    forbidden = ["source_uri", "rtsp_url", "password"]
    assert all(token not in source for token in forbidden), "Qt client must not send camera secrets"


def run_contract(base: str) -> None:
    status, health = json_request(base, "GET", "/api/v1/health")
    assert status == 200 and health["success"] and health["demo_mode"]

    status, rejected = json_request(base, "POST", "/api/v1/people-flow/start", {
        "camera_profile": "entry_camera_01",
        "camera_id": "entry_camera_01",
        "rtsp_url": "rtsp://must-not-be-accepted",
    })
    assert status == 400 and rejected["error_code"] == "RTSP_URI_IN_REQUEST_FORBIDDEN"

    status, started = json_request(base, "POST", "/api/v1/people-flow/start", {
        "camera_profile": "entry_camera_01",
        "camera_id": "entry_camera_01",
        "config_version": "entry-line-v3-security-demo",
        "initial_occupancy": 2,
    })
    assert status == 202 and started["session_id"].startswith("pf_demo_")
    session_id = started["session_id"]

    status, duplicate = json_request(base, "POST", "/api/v1/people-flow/start", {
        "camera_profile": "entry_camera_01", "camera_id": "entry_camera_01"
    })
    assert status == 409 and duplicate["error_code"] == "CAMERA_ALREADY_ACTIVE"

    status, current = json_request(base, "GET", f"/api/v1/people-flow/{session_id}/status")
    assert status == 200 and current["status"] == "running"
    assert current["flow"]["occupancy"] == 2

    status, realtime = json_request(base, "GET", "/api/v1/people-flow/cameras/entry_camera_01/realtime")
    assert status == 200 and realtime["session_id"] == session_id

    status, snapshot, content_type = request(base, "GET", f"/api/v1/people-flow/{session_id}/snapshot")
    assert status == 200 and content_type == "image/jpeg" and len(snapshot) > 100

    status, security = json_request(base, "GET", f"/api/v1/people-flow/{session_id}/security")
    assert status == 200 and set(security["stages"]) == {"phase1", "phase2", "phase3", "phase4"}
    assert security["stages"]["phase4"]["demo_classifier"] is True

    time.sleep(3.1)
    status, events = json_request(base, "GET", "/api/v1/people-flow/cameras/entry_camera_01/events?limit=50")
    assert status == 200 and len(events["events"]) >= 1

    status, stopped = json_request(base, "POST", f"/api/v1/people-flow/{session_id}/stop")
    assert status == 200 and stopped["stop_requested"]
    status, final = json_request(base, "GET", f"/api/v1/people-flow/{session_id}/status")
    assert status == 200 and final["status"] == "stopped"


def main() -> int:
    verify_qt_endpoint_wiring()
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    process = subprocess.Popen(
        [sys.executable, str(MOCK_SERVER), "--port", str(port)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_ready(base)
        run_contract(base)
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    assert process.returncode in (0, 1, -15), process.returncode
    print("PASS: Qt demo API workflow and client endpoint wiring are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
