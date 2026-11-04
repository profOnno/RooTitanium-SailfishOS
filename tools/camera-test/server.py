#!/usr/bin/env python3
"""Tiny HTTPS server for the RooTitanium camera test page.

Why a server at all: getUserMedia only works in a secure context, so the phone
must load the page over HTTPS. This script serves ../index.html (same directory)
and accepts the JSON report the page POSTs at the end, saving it under reports/
together with one JPEG per snapshot.

    python3 server.py                 # HTTPS on :8443, self-signed cert
    python3 server.py --port 9443
    python3 server.py --http          # only for localhost experiments

On the phone open https://<this-machine-lan-ip>:8443/ and accept the certificate
warning once. Reports land in ./reports/ (JSON + snapshots). Nothing is sent
anywhere else.
"""
import argparse
import base64
import datetime
import http.server
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
REPORTS = os.path.join(HERE, "reports")
MAX_BODY = 64 * 1024 * 1024  # snapshots make the JSON a few hundred KB


def lan_ip():
    """Best-effort primary LAN address (no packet is actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def ensure_cert(cert, key, hosts):
    if os.path.exists(cert) and os.path.exists(key):
        return
    san = ["DNS:localhost", "DNS:" + socket.gethostname()]
    for h in hosts:
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", h):
            san.append("IP:" + h)
    cmd = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", key, "-out", cert, "-days", "365",
        "-subj", "/CN=rootitanium-camera-test",
        "-addext", "subjectAltName=" + ",".join(san),
    ]
    print("[cert] generating self-signed certificate for", ", ".join(san))
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        # Older OpenSSL without -addext: retry without SAN (the browser warns harder).
        print("[cert] -addext failed, retrying without SAN:", e.stderr.decode(errors="replace").strip())
        subprocess.run(cmd[:cmd.index("-addext")] + cmd[cmd.index("-addext") + 2:], check=True)


def safe_name(text, fallback="snap"):
    text = re.sub(r"[^A-Za-z0-9._-]+", "-", str(text or "")).strip("-")
    return (text or fallback)[:60]


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path.rstrip("/")
        if not path.endswith("report"):
            self.send_error(404, "POST to /report only")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self.send_error(413, "missing or too large body")
            return
        body = self.rfile.read(length)
        try:
            data = json.loads(body.decode("utf-8"))
        except Exception as e:
            self.send_error(400, "invalid JSON: %s" % e)
            return

        os.makedirs(REPORTS, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        rid = safe_name(data.get("runId"), "run")
        base = f"{stamp}-{rid}"
        json_path = os.path.join(REPORTS, base + ".json")
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)

        saved = 0
        for i, snap in enumerate(data.get("snapshots") or []):
            url = snap.get("dataUrl") or ""
            m = re.match(r"^data:image/(jpeg|jpg|png);base64,(.*)$", url, re.S)
            if not m:
                continue
            ext = "png" if m.group(1) == "png" else "jpg"
            name = f"{base}-{i:02d}-{safe_name(snap.get('label'), 'snap')}.{ext}"
            with open(os.path.join(REPORTS, name), "wb") as f:
                f.write(base64.b64decode(m.group(2)))
            saved += 1

        env = data.get("env") or {}
        summary = data.get("summary") or {}
        print(f"[report] {json_path}  ({len(body)} bytes, {saved} snapshot(s))")
        print(f"         UA: {env.get('userAgent', '?')[:90]}")
        print(f"         tests={summary.get('tests')} failed={summary.get('failed')} "
              f"cameras={len(data.get('devices') or [])}")

        msg = f"saved {os.path.basename(json_path)}"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write((msg + "\n").encode())

    def log_message(self, fmt, *args):
        print("[http]", fmt % args)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--http", action="store_true", help="plain HTTP (localhost only)")
    ap.add_argument("--cert", default=os.path.join(HERE, "cert.pem"))
    ap.add_argument("--key", default=os.path.join(HERE, "key.pem"))
    args = ap.parse_args()

    ip = lan_ip()
    cert, key = args.cert, args.key
    scheme = "http"
    if not args.http:
        scheme = "https"
        if not (os.path.exists(cert) and os.path.exists(key)):
            print("[cert] no certificate found; generating a self-signed one")
            ensure_cert(cert, key, [ip, "127.0.0.1"])

    httpd = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    if not args.http:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print()
    print(f"  camera test:  {scheme}://{ip}:{args.port}/")
    print(f"  also:         {scheme}://localhost:{args.port}/")
    print(f"  reports:      {REPORTS}")
    if not args.http:
        print("  certificate is self-signed: accept the warning on the phone once.")
    print("  Ctrl-C to stop.")
    print()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
