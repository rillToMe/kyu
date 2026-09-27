"""TLS twin of serve.py for the KyuzenOS browser TLS tests.

Serves tests/browser_site/ over HTTPS on 0.0.0.0:PORT (default 8772)
with tests/browser_site/certs/server.crt + server.key (issued by
tests/browser_site/certs/testca.crt). Same routes as serve.py:
  /redir -> 302 to /page2.html
  /loop1 <-> /loop2 (redirect-limit test)
Everything else -> static files.
"""
import http.server
import os
import ssl
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
CERTS = os.path.join(ROOT, "certs")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8772


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, format, *args):
        sys.stdout.write("[tls-fixture] " + format % args + "\n")
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
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(CERTS, "server.crt"),
                        os.path.join(CERTS, "server.key"))
    srv = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("[tls-fixture] serving %s on 127.0.0.1:%d" % (ROOT, PORT), flush=True)
    srv.serve_forever()
