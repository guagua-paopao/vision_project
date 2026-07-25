from __future__ import annotations

import argparse
import json
import pathlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


ROOT = pathlib.Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web/camera-admin"
JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb0043000503040404030504040405050506070c"
    "08070707070f0b0b090c110f1212110f111113161c1713141a1511111821181a1d1d1f1f1f131722"
    "24221e241c1e1f1effdb0043010505050706070e08080e1e1411141e1e1e1e1e1e1e1e1e1e1e1e"
    "1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e"
    "1effc00011080004000403012200021101031101ffc4001f0000010501010101010100000000000000"
    "0102030405060708090a0bffc400b5100002010303020403050504040000017d010203000411051221"
    "31410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a"
    "3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a8384"
    "85868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7"
    "c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f010003"
    "0101010101010101010000000000000102030405060708090a0bffc400b51100020102040403040705"
    "040400010277000102031104052131061241510761711322328108144291a1b1c109233352f0156272"
    "d10a162434e125f11718191a262728292a35363738393a434445464748494a535455565758595a6364"
    "65666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9"
    "aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2"
    "f3f4f5f6f7f8f9faffda000c03010002110311003f00e4a8a28afd20fcbcffd9"
)

CAMERA = {
    "camera_id": "entry_camera_01",
    "name": "主入口",
    "camera_profile": "entry_camera_01",
    "enabled": True,
    "frame_interval_ms": 1000,
    "output_mode": "both",
    "jpeg_quality": 90,
    "max_width": 1280,
    "max_height": 720,
    "retention_days": 7,
    "max_saved_frames": 1000,
    "desired_state": "running",
    "analysis": {
        "enabled": True,
        "target_infer_fps": 5.0,
        "algorithm_profile": "security_default",
        "algorithms": ["people_flow", "security"],
    },
    "callback_profile": "backend_primary",
    "version": 3,
    "current_run": {"run_id": "cr_web_r6", "status": "running"},
}


class Handler(BaseHTTPRequestHandler):
    server_version = "VisionR6Mock/1.0"

    def log_message(self, *_args: object) -> None:
        return

    def send_security_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; base-uri 'none'; object-src 'none'; "
            "frame-ancestors 'none'; form-action 'self'; "
            "img-src 'self' blob:; style-src 'self'; "
            "script-src 'self'; connect-src 'self'",
        )

    def send_json(self, status: int, value: object) -> None:
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_security_headers()
        self.end_headers()
        self.wfile.write(body)

    def authorized(self) -> bool:
        return self.headers.get("Authorization") == "Bearer demo-token"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if path in ("/camera-admin", "/camera-admin/"):
            return self.send_asset("index.html", "text/html; charset=utf-8")
        if path == "/camera-admin/app.js":
            return self.send_asset(
                "app.js", "application/javascript; charset=utf-8"
            )
        if path == "/camera-admin/styles.css":
            return self.send_asset("styles.css", "text/css; charset=utf-8")
        if path == "/api/v1/health":
            return self.send_json(200, {"success": True})
        if path == "/api/v1/ready":
            return self.send_json(
                200,
                {
                    "success": True,
                    "ready": True,
                    "expected_runtime_mode": "unified_camera_pipeline",
                    "worker_mode_consistent": True,
                    "single_vision_worker": True,
                    "legacy_people_flow_worker_detected": False,
                    "worker_coordination_healthy": True,
                },
            )
        if not self.authorized():
            return self.send_json(
                401, {"success": False, "error_code": "UNAUTHORIZED"}
            )
        if path == "/api/v1/cameras":
            return self.send_json(200, {"success": True, "items": [CAMERA]})
        if path == "/api/v1/camera-profiles":
            return self.send_json(
                200,
                {
                    "success": True,
                    "items": [
                        {
                            "profile_id": "entry_camera_01",
                            "display_name": "主入口",
                            "url_env": "ENTRY_CAMERA_RTSP_URL",
                            "transport": "tcp",
                            "enabled": True,
                        }
                    ],
                },
            )
        if path == "/api/v1/camera-hubs":
            return self.send_json(
                200,
                {
                    "success": True,
                    "items": [
                        {
                            "camera_profile": "entry_camera_01",
                            "hub_instance_id": "hub_r6",
                            "state": "running",
                            "open_count": 1,
                            "subscriber_count": 1,
                            "subscriber_types": {"camera_pipeline": 1},
                            "capture_fps": 25.0,
                            "reconnect_count": 0,
                        }
                    ],
                },
            )
        if path == "/api/v1/cameras/entry_camera_01":
            return self.send_json(200, {"success": True, "camera": CAMERA})
        if path == "/api/v1/cameras/entry_camera_01/status":
            return self.send_json(
                200,
                {
                    "success": True,
                    "camera_id": "entry_camera_01",
                    "run_id": "cr_web_r6",
                    "status": "running",
                    "runtime_stale": False,
                    "pipeline": {"thread_running": True, "sample_fps": 1.0},
                    "hub": {"state": "running", "capture_fps": 25.0},
                    "analysis": {
                        "enabled": True,
                        "state": "running",
                        "runtime_stale": False,
                        "config_version": "entry-line-v3",
                        "infer_fps": 5.2,
                        "initial_occupancy": 7,
                        "in_count": 12,
                        "out_count": 5,
                        "occupancy": 14,
                        "live_persons": 3,
                        "reconnect_count": 0,
                        "warmup_frames_remaining": 0,
                        "security": {
                            "stages": {
                                "phase1": {
                                    "name": "electronic_fence",
                                    "ready": True,
                                },
                                "phase2": {
                                    "name": "tracking",
                                    "ready": True,
                                },
                                "phase3": {
                                    "name": "pose_action",
                                    "ready": True,
                                },
                                "phase4": {
                                    "name": "temporal_action_demo",
                                    "ready": True,
                                    "demo_classifier": True,
                                },
                            }
                        },
                    },
                },
            )
        if path in (
            "/api/v1/cameras/entry_camera_01/analysis-snapshot",
            "/api/v1/cameras/entry_camera_01/latest-frame",
        ):
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(JPEG)))
            self.send_header("Cache-Control", "no-store")
            self.send_security_headers()
            self.end_headers()
            self.wfile.write(JPEG)
            return
        if path == "/api/v1/cameras/entry_camera_01/alerts":
            return self.send_json(
                200,
                {
                    "success": True,
                    "items": [
                        {
                            "event_id": "evt_r6",
                            "run_id": "cr_web_r6",
                            "event_type": "line_crossing_in",
                            "category": "people_flow",
                            "severity": 2,
                            "track_id": 42,
                            "occurred_at_ms": 1784973600000,
                            "algorithm": {
                                "profile": "security_default",
                                "config_version": "entry-line-v3",
                            },
                            "delivery": {"status": "delivered"},
                        }
                    ],
                },
            )
        if path == "/api/v1/operations/metrics":
            return self.send_json(
                200,
                {
                    "success": True,
                    "runs": {"active": 1},
                    "alerts": {"total": 1},
                    "algorithm_runtime": {
                        "available": True,
                        "stale": False,
                    },
                },
            )
        return self.send_json(
            404, {"success": False, "error_code": "NOT_FOUND"}
        )

    def send_asset(self, name: str, content_type: str) -> None:
        body = (WEB_ROOT / name).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_security_headers()
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18087)
    args = parser.parse_args()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
