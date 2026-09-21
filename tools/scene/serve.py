# Serves the three.js test scene and receives exported frames.
#   python tools/scene/serve.py [port]        then open http://localhost:8765/
# GET  /...            static files from this folder
# POST /save?name=X    body -> tools/scene/out/X   (frames and motion vectors from ?export=1)
# Local only (127.0.0.1); names are flattened so a request cannot write outside out/.
import http.server, os, sys, urllib.parse

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "out")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path != "/save":
            self.send_error(404); return
        name = os.path.basename(urllib.parse.parse_qs(u.query).get("name", ["frame.bin"])[0])
        sub = os.path.basename(urllib.parse.parse_qs(u.query).get("dir", [""])[0])
        d = os.path.join(OUT, sub) if sub else OUT
        os.makedirs(d, exist_ok=True)
        n = int(self.headers.get("Content-Length", "0"))
        with open(os.path.join(d, name), "wb") as f:
            f.write(self.rfile.read(n))
        self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if "/save" in (args[0] if args else ""):
            return   # one line per frame is noise
        super().log_message(fmt, *args)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"test scene on http://localhost:{port}/   (exports -> {OUT})", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
