"""Runner for the host-side TLS test (make test-tls).

Starts the TLS fixture server, waits for the port, runs the test
binary, then kills the server. Exit code = test binary's.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(ROOT))
PORT = 8772
# The test binary lives next to its source for the Make build. The CMake build
# puts it under build/host/bin, so an explicit override is honoured when set.
EXE = os.environ.get("KYUZEN_TLS_TEST") or os.path.join(
    REPO, "tests", "host", "unit", "tls_test")
if os.name == "nt" and not os.path.exists(EXE):
    EXE += ".exe"


def wait_port():
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


if __name__ == "__main__":
    srv = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tls_serve.py"), str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        if not wait_port():
            print("[tls-test] fixture server did not start", flush=True)
            sys.exit(2)
        r = subprocess.run([EXE, "127.0.0.1", str(PORT)])
        sys.exit(r.returncode)
    finally:
        srv.kill()
