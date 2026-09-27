"""Record the ClientHellos a sing-box binary sends for one outbound.

Runs the binary with a mixed inbound routed to the outbound under test, points
the outbound at a local listener, opens N connections through the inbound and
saves the first TLS record of each as <out>/<name>.<n>.hex. Nothing leaves the
machine: the listener closes the connection after the ClientHello.

    python capture.py <sing-box.exe> <case> <out-dir> [runs]

Cases (the outbound's TLS options; server name and keys are throwaway):
    anytls-reality-chrome   AnyTLS + REALITY, uTLS chrome
    anytls-tls-chrome       AnyTLS over plain TLS, uTLS chrome
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

CAPTURE_PORT = 18443
MIXED_PORT = 20809
# A fixed, throwaway REALITY public key: the client only needs a valid x25519
# point, and a fixed one keeps the generated config identical between runs.
REALITY_PUBLIC_KEY = "jNXHt1yRo0vDuchQlIP6Z0ZvjT3KtzVI-T4E7RoLJS0"


def outbound(case):
    tls = {"enabled": True, "server_name": "www.ebay.com",
           "utls": {"enabled": True, "fingerprint": "chrome"}}
    if case == "anytls-reality-chrome":
        tls["reality"] = {"enabled": True, "public_key": REALITY_PUBLIC_KEY, "short_id": "0123abcd"}
    elif case != "anytls-tls-chrome":
        sys.exit(f"unknown case {case}")
    return {"type": "anytls", "tag": "out", "server": "127.0.0.1", "server_port": CAPTURE_PORT,
            "password": "conformance", "tls": tls}


def record_from(conn):
    conn.settimeout(5)
    data = b""
    try:
        while len(data) < 5 or len(data) < 5 + int.from_bytes(data[3:5], "big"):
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    if len(data) < 5 or data[0] != 0x16:
        return None
    return data[: 5 + int.from_bytes(data[3:5], "big")]


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    binary, case, out_dir = sys.argv[1:4]
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    os.makedirs(out_dir, exist_ok=True)

    captured = []
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", CAPTURE_PORT))
    server.listen(8)

    def listen():
        while len(captured) < runs:
            conn, _ = server.accept()
            record = record_from(conn)
            conn.close()
            if record:
                captured.append(record)

    threading.Thread(target=listen, daemon=True).start()

    config = {"log": {"level": "error"},
              "inbounds": [{"type": "mixed", "tag": "in", "listen": "127.0.0.1", "listen_port": MIXED_PORT}],
              "outbounds": [outbound(case)], "route": {"final": "out"}}
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(config, f)
    box = subprocess.Popen([binary, "run", "-c", path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(2)
        for n in range(runs):
            client = socket.create_connection(("127.0.0.1", MIXED_PORT), timeout=5)
            client.sendall(b"CONNECT example.com:80 HTTP/1.1\r\nHost: example.com:80\r\n\r\n")
            try:
                client.recv(100)
            except OSError:
                pass
            client.close()
            deadline = time.time() + 5
            while len(captured) <= n and time.time() < deadline:
                time.sleep(0.05)
    finally:
        box.kill()
        os.remove(path)

    if len(captured) != runs:
        sys.exit(f"captured {len(captured)} of {runs} ClientHellos")
    for n, record in enumerate(captured, 1):
        with open(os.path.join(out_dir, f"{case}.{n}.hex"), "w") as f:
            f.write(record.hex() + "\n")
    print(f"{case}: {runs} ClientHellos -> {out_dir}")


if __name__ == "__main__":
    main()
