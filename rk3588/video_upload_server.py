#!/usr/bin/env python3
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse
import cgi
import json
import shutil
import time

ROOT = Path.home() / "data" / "camera_flow" / "video_segments"
TOKEN = "CHANGE_ME_TOKEN"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if urlparse(self.path).path != "/api/video/upload":
            self.send_json(404, {"code": 404, "message": "not found"})
            return

        auth = self.headers.get("Authorization", "")
        if auth != f"Bearer {TOKEN}":
            self.send_json(401, {"code": 401, "message": "bad token"})
            return

        form = cgi.FieldStorage(
            fp=self.rfile,
            headers=self.headers,
            environ={
                "REQUEST_METHOD": "POST",
                "CONTENT_TYPE": self.headers.get("Content-Type"),
            },
        )

        device_id = form.getfirst("device_id", "unknown")
        start_ms = int(form.getfirst("start_ms", "0") or "0")
        file_item = form["file"] if "file" in form else None

        if file_item is None or not getattr(file_item, "filename", ""):
            self.send_json(400, {"code": 400, "message": "missing file"})
            return

        if start_ms > 0:
            timestamp = time.localtime(start_ms / 1000)
        else:
            timestamp = time.localtime()

        date_dir = time.strftime("%Y-%m-%d", timestamp)
        safe_device_id = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in device_id)
        safe_name = Path(file_item.filename).name

        target_dir = ROOT / safe_device_id / date_dir
        target_dir.mkdir(parents=True, exist_ok=True)
        target_path = target_dir / safe_name

        with target_path.open("wb") as out:
            shutil.copyfileobj(file_item.file, out)

        print(f"[UPLOAD] {safe_device_id} -> {target_path}", flush=True)
        self.send_json(200, {"code": 0, "remote_path": str(target_path)})

    def log_message(self, fmt, *args):
        print("[HTTP] " + fmt % args, flush=True)

    def send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ROOT.mkdir(parents=True, exist_ok=True)
    addr = ("0.0.0.0", 8080)
    print(f"Video upload server listening on http://{addr[0]}:{addr[1]}/api/video/upload", flush=True)
    print(f"Save root: {ROOT}", flush=True)
    HTTPServer(addr, Handler).serve_forever()
