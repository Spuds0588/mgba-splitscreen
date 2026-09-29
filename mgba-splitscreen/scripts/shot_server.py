#!/usr/bin/env python3
"""Tiny CORS-enabled PNG receiver: POST a body, write it to docs/screens/.

Used by the screenshot sessions: the page (served from a different origin) POSTs
PNG bytes to /<name>.png and this writes them into the repo's docs/screens/
directory. GET /ping answers ok for readiness checks.
"""
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "docs", "screens")
os.makedirs(OUT, exist_ok=True)


class Handler(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/ping"):
            body = b"ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self._cors()
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self._cors()
        self.end_headers()

    def do_POST(self):
        name = os.path.basename(self.path.split("?")[0])
        if not name.endswith(".png"):
            self.send_response(400)
            self._cors()
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        path = os.path.join(OUT, name)
        with open(path, "wb") as fh:
            fh.write(data)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        body = f"saved {name} ({length} bytes)".encode()
        self.send_header("Content-Length", str(len(body)))
        self._cors()
        self.end_headers()
        self.wfile.write(body)
        print(f"[shot_server] saved {name} ({length} bytes)")

    def log_message(self, fmt, *args):
        pass  # keep the console quiet; successful saves print their own line


if __name__ == "__main__":
    print(f"[shot_server] writing PNGs to {OUT}")
    ThreadingHTTPServer(("127.0.0.1", 8091), Handler).serve_forever()
