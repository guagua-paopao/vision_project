#!/usr/bin/env python3
"""A dependency-free People Flow API mock for the Qt workflow demo.

This server is intentionally limited to local UI integration. It does not open
RTSP streams, run inference, persist data, or replace the real C++ service.
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


FALLBACK_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////"
    "2wBDAf//////////////////////////////////////////////////////////////////////////////////////"
    "wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "DAMBAAIQAxAAAAF//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABBQJ//8QAFBEBAAAAAAAAAAAAAAAAAAAA"
    "AP/aAAgBAwEBPwF//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPwF//8QAFBABAAAAAAAAAAAAAAAAAAAA"
    "AP/aAAgBAQAGPwJ//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPyF//9oADAMBAAIAAwAAABCf/8QAFBEBA"
    "AAAAAAAAAAAAAAAAAAAAP/aAAgBAwEBPxB//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPxB//8QAFBAB"
    "AAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxB//9k="
)


def now_ms() -> int:
    return int(time.time() * 1000)


class DemoState:
    def __init__(self, snapshot_path: Path | None) -> None:
        self.lock = threading.Lock()
        self.snapshot = self._load_snapshot(snapshot_path)
        self.session_id = ""
        self.camera_id = "entry_camera_01"
        self.camera_profile = "entry_camera_01"
        self.config_version = "entry-line-v3-security-demo"
        self.status = "idle"
        self.initial_occupancy = 0
        self.started_ms = 0
        self.stopped_ms = 0
        self.events: list[dict[str, Any]] = []

    @staticmethod
    def _load_snapshot(path: Path | None) -> bytes:
        if path and path.is_file():
            return path.read_bytes()
        default_path = Path(__file__).resolve().parents[1] / "videos" / "acceptance_calibration" / "frame_013.jpg"
        if default_path.is_file():
            return default_path.read_bytes()
        return FALLBACK_JPEG

    def start(self, body: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        forbidden = {"source_uri", "rtsp_url", "password"}.intersection(body)
        if forbidden:
            return 400, {
                "success": False,
                "error_code": "RTSP_URI_IN_REQUEST_FORBIDDEN",
                "error": "camera credentials are accepted only through the configured environment variable",
            }
        with self.lock:
            if self.status == "running":
                return 409, {
                    "success": False,
                    "error_code": "CAMERA_ALREADY_ACTIVE",
                    "error": "the demo camera already has an active session",
                    "active_session_id": self.session_id,
                }
            self.camera_profile = str(body.get("camera_profile", "entry_camera_01"))
            self.camera_id = str(body.get("camera_id", "entry_camera_01"))
            self.config_version = str(body.get("config_version", "entry-line-v3-security-demo"))
            self.initial_occupancy = max(0, int(body.get("initial_occupancy", 0)))
            self.started_ms = now_ms()
            self.stopped_ms = 0
            self.session_id = f"pf_demo_{self.started_ms}"
            self.status = "running"
            self.events = []
            return 202, {
                "success": True,
                "session_id": self.session_id,
                "camera_id": self.camera_id,
                "camera_profile": self.camera_profile,
                "config_version": self.config_version,
                "status": "queued",
                "status_url": f"/api/v1/people-flow/{self.session_id}/status",
                "snapshot_url": f"/api/v1/people-flow/{self.session_id}/snapshot",
                "security_url": f"/api/v1/people-flow/{self.session_id}/security",
                "stop_url": f"/api/v1/people-flow/{self.session_id}/stop",
                "realtime_url": f"/api/v1/people-flow/cameras/{self.camera_id}/realtime",
                "demo_mode": True,
            }

    def stop(self, session_id: str) -> tuple[int, dict[str, Any]]:
        with self.lock:
            if session_id != self.session_id:
                return 404, {"success": False, "error_code": "SESSION_NOT_FOUND"}
            if self.status != "running":
                return 409, {"success": False, "error_code": "SESSION_ALREADY_FINISHED"}
            self._sync_events()
            self.status = "stopped"
            self.stopped_ms = now_ms()
            return 200, {
                "success": True,
                "session_id": self.session_id,
                "camera_id": self.camera_id,
                "status": "stopping",
                "stop_requested": True,
                "demo_mode": True,
            }

    def _sync_events(self) -> None:
        if not self.started_ms:
            return
        end_ms = self.stopped_ms or now_ms()
        desired = min(20, max(0, (end_ms - self.started_ms) // 3000))
        while len(self.events) < desired:
            index = len(self.events) + 1
            direction = "IN" if index % 3 != 0 else "OUT"
            self.events.append({
                "event_id": f"demo_event_{index:04d}",
                "session_id": self.session_id,
                "camera_id": self.camera_id,
                "line_id": "entrance_line_01",
                "track_id": 100 + index,
                "direction": direction,
                "event_time_ms": self.started_ms + index * 3000,
                "confidence": round(0.91 - (index % 4) * 0.03, 2),
                "point_x_norm": 0.48,
                "point_y_norm": 0.62,
                "config_version": self.config_version,
                "evidence_path": "",
            })

    def counts(self) -> tuple[int, int, int]:
        self._sync_events()
        count_in = sum(event["direction"] == "IN" for event in self.events)
        count_out = sum(event["direction"] == "OUT" for event in self.events)
        return count_in, count_out, max(0, self.initial_occupancy + count_in - count_out)

    def status_payload(self, session_id: str) -> tuple[int, dict[str, Any]]:
        with self.lock:
            if session_id != self.session_id:
                return 404, {"success": False, "error_code": "SESSION_NOT_FOUND"}
            count_in, count_out, occupancy = self.counts()
            elapsed = max(0, now_ms() - self.started_ms)
            return 200, {
                "success": self.status != "failed",
                "session_id": self.session_id,
                "camera_id": self.camera_id,
                "camera_profile": self.camera_profile,
                "config_version": self.config_version,
                "status": self.status,
                "stop_requested": self.status == "stopped",
                "capture": {
                    "connected": self.status == "running",
                    "state": "running" if self.status == "running" else "stopped",
                    "backend": "demo",
                    "width": 640,
                    "height": 480,
                    "capture_fps": 24.8 if self.status == "running" else 0,
                    "source_fps": 25.0,
                    "last_frame_age_ms": 42,
                    "dropped_frames": 0,
                    "reconnect_count": 0,
                    "resolution_changed": False,
                },
                "inference": {
                    "infer_fps": 10.0 if self.status == "running" else 0,
                    "last_inference_ms": 11.7,
                    "frame_count": elapsed // 100,
                    "live_persons": (elapsed // 2000) % 4 if self.status == "running" else 0,
                },
                "flow": {"in": count_in, "out": count_out, "occupancy": occupancy,
                         "applied_calibration_version": 0},
                "storage": {"degraded": False, "event_queue_depth": 0, "snapshot_degraded": False},
                "create_time_ms": self.started_ms,
                "start_time_ms": self.started_ms,
                "stop_time_ms": self.stopped_ms,
                "last_update_ms": now_ms(),
                "demo_mode": True,
            }

    def realtime_payload(self, camera_id: str) -> tuple[int, dict[str, Any]]:
        with self.lock:
            if camera_id != self.camera_id or not self.session_id:
                return 404, {"success": False, "error_code": "CAMERA_NOT_FOUND"}
            count_in, count_out, occupancy = self.counts()
            elapsed = max(0, now_ms() - self.started_ms)
            return 200, {
                "success": True,
                "camera_id": self.camera_id,
                "session_id": self.session_id,
                "status": self.status,
                "in": count_in,
                "out": count_out,
                "occupancy": occupancy,
                "live_persons": (elapsed // 2000) % 4 if self.status == "running" else 0,
                "frame_count": elapsed // 100,
                "capture_fps": 24.8 if self.status == "running" else 0,
                "infer_fps": 10.0 if self.status == "running" else 0,
                "source_fps": 25.0,
                "latest_frame_age_ms": 42,
                "reconnect_count": 0,
                "dropped_frames": 0,
                "storage_degraded": False,
                "event_queue_depth": 0,
                "last_update_ms": now_ms(),
                "config_version": self.config_version,
                "demo_mode": True,
            }

    def events_payload(self, camera_id: str, limit: int) -> tuple[int, dict[str, Any]]:
        with self.lock:
            if camera_id != self.camera_id:
                return 404, {"success": False, "error_code": "CAMERA_NOT_FOUND"}
            self._sync_events()
            items = list(reversed(self.events[-limit:]))
            return 200, {
                "success": True,
                "camera_id": camera_id,
                "direction": "",
                "limit": limit,
                "offset": 0,
                "count": len(items),
                "events": items,
                "demo_mode": True,
            }

    def security_payload(self, session_id: str) -> tuple[int, dict[str, Any]]:
        with self.lock:
            if session_id != self.session_id:
                return 404, {"success": False, "error_code": "SESSION_NOT_FOUND"}
            elapsed = max(0, now_ms() - self.started_ms)
            pose_count = 1 + (elapsed // 4000) % 2 if self.status == "running" else 0
            inside = 1 if (elapsed // 5000) % 2 else 0
            events = [
                {"event_id": "zone_demo_1", "category": "zone", "event_type": "ZONE_ENTER",
                 "track_id": 101, "zone_id": "restricted_demo", "event_time_ms": self.started_ms + 1000,
                 "confidence": 0.93, "severity": 2, "demo_classifier": False},
                {"event_id": "pose_demo_1", "category": "pose_action", "event_type": "HANDS_UP_START",
                 "track_id": 101, "zone_id": "", "event_time_ms": self.started_ms + 2000,
                 "confidence": 0.91, "severity": 2, "demo_classifier": False},
                {"event_id": "temporal_demo_1", "category": "temporal_action",
                 "event_type": "RAPID_MOTION_DEMO_START", "track_id": 101, "zone_id": "",
                 "event_time_ms": self.started_ms + 3000, "confidence": 0.86,
                 "severity": 3, "demo_classifier": True},
            ]
            visible_events = [event for event in events if event["event_time_ms"] <= now_ms()]
            return 200, {
                "success": True,
                "mode": "single_machine_four_stage_demo",
                "production_action_model": False,
                "session_id": self.session_id,
                "camera_id": self.camera_id,
                "timestamp_ms": now_ms(),
                "stages": {
                    "phase1": {"name": "electronic_fence", "ready": True,
                               "zone_count": 1, "inside_count": inside, "statuses": []},
                    "phase2": {"name": "tracking_and_analytics", "ready": True,
                               "alpha_beta_filter": True, "track_count": pose_count, "tracks": []},
                    "phase3": {"name": "pose_rule_actions", "ready": True,
                               "model": "yolo11-pose-tensorrt", "pose_count": pose_count,
                               "active_actions": [{"track_id": 101, "labels": ["HANDS_UP"]}]
                               if elapsed >= 2000 else []},
                    "phase4": {"name": "temporal_action_demo", "ready": True,
                               "classifier": "feature-threshold-demo", "demo_classifier": True,
                               "label": "RAPID_MOTION_DEMO",
                               "active_actions": [{"track_id": 101, "labels": ["RAPID_MOTION_DEMO"]}]
                               if elapsed >= 3000 else []},
                },
                "events": list(reversed(visible_events)),
                "demo_mode": True,
            }


class PeopleFlowDemoHandler(BaseHTTPRequestHandler):
    server_version = "PeopleFlowDemo/0.1"

    @property
    def state(self) -> DemoState:
        return self.server.demo_state  # type: ignore[attr-defined]

    def log_message(self, message: str, *args: Any) -> None:
        print(f"[{self.log_date_time_string()}] {message % args}")

    def _json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            return json.loads(self.rfile.read(length) or b"{}")
        except (ValueError, json.JSONDecodeError):
            return {}

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/api/v1/health":
            self._json(200, {"success": True, "people_flow_enabled": True,
                             "people_flow_storage": {"started": True, "degraded": False},
                             "demo_mode": True})
            return
        if path == "/api/v1/ready":
            self._json(200, {"success": True, "ready": True, "demo_mode": True})
            return
        if path == "/api/v1/workers":
            self._json(200, {"success": True, "workers": [{"name": "demo_worker", "alive": True}],
                             "demo_mode": True})
            return
        if path == "/api/v1/metrics":
            self._json(200, {"success": True, "demo_mode": True, "infer_fps": 10.0})
            return

        match = re.fullmatch(r"/api/v1/people-flow/([^/]+)/status", path)
        if match:
            status, payload = self.state.status_payload(match.group(1))
            self._json(status, payload)
            return
        match = re.fullmatch(r"/api/v1/people-flow/([^/]+)/snapshot", path)
        if match:
            if match.group(1) != self.state.session_id:
                self._json(404, {"success": False, "error_code": "SESSION_NOT_FOUND"})
                return
            body = self.state.snapshot
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        match = re.fullmatch(r"/api/v1/people-flow/([^/]+)/security", path)
        if match:
            status, payload = self.state.security_payload(match.group(1))
            self._json(status, payload)
            return
        match = re.fullmatch(r"/api/v1/people-flow/cameras/([^/]+)/realtime", path)
        if match:
            status, payload = self.state.realtime_payload(match.group(1))
            self._json(status, payload)
            return
        match = re.fullmatch(r"/api/v1/people-flow/cameras/([^/]+)/events", path)
        if match:
            query = parse_qs(parsed.query)
            try:
                limit = min(1000, max(1, int(query.get("limit", ["50"])[0])))
            except ValueError:
                self._json(400, {"success": False, "error_code": "INVALID_QUERY_RANGE"})
                return
            status, payload = self.state.events_payload(match.group(1), limit)
            self._json(status, payload)
            return
        match = re.fullmatch(r"/api/v1/people-flow/cameras/([^/]+)/summary", path)
        if match:
            status, events_payload = self.state.events_payload(match.group(1), 1000)
            if status != 200:
                self._json(status, events_payload)
                return
            events = events_payload["events"]
            count_in = sum(event["direction"] == "IN" for event in events)
            count_out = sum(event["direction"] == "OUT" for event in events)
            self._json(200, {"success": True, "camera_id": match.group(1), "bucket": "minute",
                             "totals": {"in": count_in, "out": count_out, "net": count_in - count_out},
                             "summary": [], "demo_mode": True})
            return
        self._json(404, {"success": False, "error_code": "ROUTE_NOT_FOUND", "path": path})

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/api/v1/people-flow/start":
            status, payload = self.state.start(self._body())
            self._json(status, payload)
            return
        match = re.fullmatch(r"/api/v1/people-flow/([^/]+)/stop", path)
        if match:
            status, payload = self.state.stop(match.group(1))
            self._json(status, payload)
            return
        self._json(404, {"success": False, "error_code": "ROUTE_NOT_FOUND", "path": path})


class DemoServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], state: DemoState) -> None:
        super().__init__(address, PeopleFlowDemoHandler)
        self.demo_state = state


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the local People Flow Qt demo API")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18087)
    parser.add_argument("--snapshot", type=Path, help="Optional JPEG used by the snapshot endpoint")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = DemoServer((args.host, args.port), DemoState(args.snapshot))
    print(f"People Flow demo API listening on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
