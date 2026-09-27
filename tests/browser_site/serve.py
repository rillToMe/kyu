"""Deterministic fixture server for the KyuzenOS browser probe.

Serves tests/browser_site/ on 127.0.0.1:PORT (default 8771):
  /redir  -> 302 to /page2.html (redirect test)
Everything else -> static files. Logs each request line to stdout.
"""
import http.server
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8771


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, format, *args):
        sys.stdout.write("[fixture] " + format % args + "\n")
        sys.stdout.flush()

    def do_GET(self):
        if self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/page2.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/loop1":
            self.send_response(302)
            self.send_header("Location", "/loop2")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/loop2":
            self.send_response(302)
            self.send_header("Location", "/loop1")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        return super().do_GET()


if __name__ == "__main__":
    # 0.0.0.0: slirp guest->10.0.2.2 harus mencapai host; bind loopback saja
    # terbukti tak menerima SYN tamu pada sebagian setup Windows/QEMU.
    srv = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    print("[fixture] serving %s on 127.0.0.1:%d" % (ROOT, PORT), flush=True)
    srv.serve_forever()
