#!/usr/bin/env python3
"""
custom-fw/wasm/serve.py - a local HTTP server for the Pocket Amp WASM workbench.

Serves the WebAssembly build and the test audio files:

    python serve.py                 # serves on 127.0.0.1:8099
    python serve.py --port 8080     # on another port
    python serve.py --no-browser    # don't open the browser automatically
"""
import argparse
import http.server
import json
import os
import socketserver
import sys
import urllib.parse
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.abspath(os.path.join(HERE, "..", "emu", "samples"))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def log_message(self, fmt, *args):
        # Keep console output quiet during normal asset requests
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def end_headers(self):
        # COOP/COEP headers for SharedArrayBuffer support in browsers
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        path = u.path

        if path == "/":
            self.send_response(302)
            self.send_header("Location", "/workbench.html")
            self.end_headers()
            return

        # Serve sample list
        if path == "/api/samples":
            names = []
            if os.path.isdir(SAMPLES):
                names = sorted(f for f in os.listdir(SAMPLES)
                               if f.lower().endswith(".wav"))
            self._json({"samples": names})
            return

        # Stream test samples
        if path.startswith("/samples/"):
            name = os.path.basename(path)
            p = os.path.join(SAMPLES, name)
            if os.path.isfile(p) and name.lower().endswith(".wav"):
                try:
                    with open(p, "rb") as f:
                        body = f.read()
                    self.send_response(200)
                    self.send_header("Content-Type", "audio/wav")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                except Exception:
                    self.send_error(500)
                    return
            self.send_error(404)
            return

        super().do_GET()


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="Pocket Amp WASM Workbench Server")
    ap.add_argument("--port", type=int, default=8099, help="port to listen on (default: 8099)")
    ap.add_argument("--host", default="127.0.0.1", help="host address (default: 127.0.0.1)")
    ap.add_argument("--no-browser", action="store_true", help="do not open browser automatically")
    a = ap.parse_args()

    wasm = os.path.join(HERE, "pocket_amp.wasm")
    if not os.path.exists(wasm):
        print("pocket_amp.wasm not found - build it first:")
        print("  python custom-fw/wasm/build_wasm.py --zig <path to zig.exe>")
        return 1

    n = len([f for f in os.listdir(SAMPLES) if f.lower().endswith(".wav")]) \
        if os.path.isdir(SAMPLES) else 0
    url = f"http://{a.host}:{a.port}/workbench.html"

    with Server((a.host, a.port), Handler) as httpd:
        print(f"  Pocket Amp Workbench  ->  {url}")
        print(f"  serving {HERE}")
        print(f"  test samples: {n} from {SAMPLES}")
        print("  Ctrl-C to stop")
        if not a.no_browser:
            try:
                webbrowser.open(url)
            except Exception:
                pass
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n  stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
