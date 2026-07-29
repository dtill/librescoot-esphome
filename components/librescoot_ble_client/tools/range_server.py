#!/usr/bin/env python3
"""Tiny Range-capable static HTTP server for OTA testing.

Python's built-in ``http.server`` ignores the ``Range`` header (always returns 200), which
breaks resume for the librescoot_ble_client OTA transfer. This one answers ``206 Partial Content``
so the ESP can resume, exactly like GitHub's asset CDN — but over plain HTTP (no TLS), which
is what makes a *local mirror* both fast and reliable on the ESP32-classic.

Usage:
    # Serve firmware bundles laid out as  <tag>/<asset-name>  (matches the OTA Source URL
    # layout  <base>/<tag>/<asset>):
    #   ./mirror/nightly-20260723T040816/librescoot-unu-mdb-nightly-20260723T040816.delta
    #   ./mirror/nightly-20260723T040816/librescoot-unu-dbc-nightly-20260723T040816.delta
    cd mirror
    python3 range_server.py [port]        # default 8000, binds 0.0.0.0

Then in Home Assistant set the component's **OTA Source URL** to
``http://<this-machine-ip>:8000`` and press Install (or use the ``ota_test`` action).

Get the real bundles for a tag with, e.g.:
    gh release download <tag> -R librescoot/librescoot -p '*mdb*.delta' -p '*dbc*.delta'
"""
import os
import re
import socketserver
import sys
from http.server import SimpleHTTPRequestHandler


class RangeHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        start, end, code = 0, size - 1, 200
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", "") or "")
        if m:
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            code = 206
        length = end - start + 1
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", port), RangeHandler) as srv:
        print(f"Range-capable mirror on http://0.0.0.0:{port}  (serving {os.getcwd()})")
        srv.serve_forever()
