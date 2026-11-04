#!/usr/bin/env python3
"""Tiny HTTPS server for the RooTitanium camera test page.

Why a server at all: getUserMedia only works in a secure context, so the phone
must load the page over HTTPS. This script serves ../index.html (same directory)
and accepts the JSON report the page POSTs at the end, saving it under reports/
together with one JPEG per snapshot.

    python3 server.py                 # HTTPS on :8443, self-signed cert, QR in console
    python3 server.py --port 9443
    python3 server.py --no-qr         # skip the console QR
    python3 server.py --url https://my.host:8443/   # override printed/QR URL
    python3 server.py --reports-dir /path/to/reports
    python3 server.py --http          # only for localhost experiments

Reports are written next to this script (./reports) by default; the folder is
created at startup and is also browsable at /reports/ from the phone.

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
import shutil
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


def print_qr(url):
    """Print a scannable QR for `url` in the terminal (best-effort).

    Backends, in order: qrencode(1) (ANSI colours on a tty, UTF-8 otherwise),
    the Python `qrcode` package, then `segno`. If none is installed the URL is
    still printed above; this function only adds the QR.
    """
    # 1) qrencode CLI (installed here; apt install qrencode)
    exe = shutil.which("qrencode")
    if exe:
        # ANSIUTF8 is background-independent and scans well on dark terminals;
        # UTF8 is cleaner when stdout is redirected to a file/log.
        modes = ["ANSIUTF8", "UTF8"] if sys.stdout.isatty() else ["UTF8"]
        for mode in modes:
            try:
                p = subprocess.run([exe, "-t", mode, "-m", "1", "-o", "-", url],
                                   capture_output=True)
            except OSError:
                break
            if p.returncode == 0 and p.stdout:
                sys.stdout.write(p.stdout.decode("utf-8", "replace"))
                if not p.stdout.endswith(b"\n"):
                    sys.stdout.write("\n")
                sys.stdout.flush()
                return True

    # 2) python qrcode
    try:
        import qrcode
        qr = qrcode.QRCode(border=1,
                           error_correction=qrcode.constants.ERROR_CORRECT_M)
        qr.add_data(url)
        qr.make(fit=True)
        qr.print_ascii(invert=True)
        sys.stdout.flush()
        return True
    except Exception:
        pass

    # 3) segno
    try:
        import segno
        segno.make(url, error="m").terminal(compact=True)
        sys.stdout.flush()
        return True
    except Exception:
        pass

    print("  (no QR backend found: install qrencode, python3-qrcode or segno)")
    return False


def main():
    global REPORTS
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--http", action="store_true", help="plain HTTP (localhost only)")
    ap.add_argument("--no-qr", action="store_true", help="do not print the QR code")
    ap.add_argument("--url", help="URL to print/encode (default: auto from the LAN IP)")
    ap.add_argument("--cert", default=os.path.join(HERE, "cert.pem"))
    ap.add_argument("--key", default=os.path.join(HERE, "key.pem"))
    ap.add_argument("--reports-dir", default=REPORTS,
                    help="where to write the JSON/JPEG reports (default: ./reports)")
    args = ap.parse_args()

    REPORTS = os.path.abspath(os.path.expanduser(args.reports_dir))
    os.makedirs(REPORTS, exist_ok=True)

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
    url = args.url or f"{scheme}://{ip}:{args.port}/"
    print(f"  camera test:  {url}")
    print(f"  also:         {scheme}://localhost:{args.port}/")
    print(f"  reports:      {REPORTS}")
    if not args.http:
        print("  certificate is self-signed: accept the warning on the phone once.")
    if not args.no_qr:
        print("  scan to open on the phone:")
        print_qr(url)
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
