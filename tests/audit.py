#!/usr/bin/env python3
"""Black-box audit regression suite shared by the Rust and C++ servers."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"
PORT = 18080
PORT2 = 18081


def request(method: str, path: str, body: bytes | None = None, headers: dict[str, str] | None = None, host: str = "localhost", port: int = PORT):
    conn = http.client.HTTPConnection(HOST, port, timeout=5)
    hdrs = {"Host": host, **(headers or {})}
    conn.request(method, path, body=body, headers=hdrs)
    resp = conn.getresponse()
    data = resp.read()
    result = (resp.status, dict(resp.getheaders()), data)
    conn.close()
    return result


def raw_http(payload: bytes, port: int = PORT) -> bytes:
    with socket.create_connection((HOST, port), timeout=5) as sock:
        sock.sendall(payload)
        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks)


def check(label: str, condition: bool):
    if not condition:
        raise AssertionError(label)
    print(f"[OK] {label}")


def make_config() -> Path:
    text = (ROOT / "localhost.conf").read_text()
    text = text.replace("127.0.0.1:8080", f"127.0.0.1:{PORT}")
    text = text.replace("127.0.0.1:8081", f"127.0.0.1:{PORT2}")
    fd, name = tempfile.mkstemp(prefix="localhost-audit-", suffix=".conf")
    os.close(fd)
    Path(name).write_text(text)
    return Path(name)


def wait_ready(proc: subprocess.Popen):
    deadline = time.time() + 8
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with {proc.returncode}")
        try:
            status, _, _ = request("GET", "/")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def run(binary: Path):
    uploads = ROOT / "public" / "uploads"
    uploads.mkdir(parents=True, exist_ok=True)
    for child in uploads.iterdir():
        if child.name != ".gitkeep" and child.is_file():
            child.unlink()

    cfg = make_config()
    proc = subprocess.Popen(
        [str(binary), "-c", str(cfg)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(proc)

        status, headers, body = request("GET", "/")
        check("GET static index", status == 200 and b"Localhost Web Server" in body)
        check("response has Content-Length", "Content-Length" in headers)

        status, _, body = request("GET", "/", port=PORT2)
        check("second configured port", status == 200 and b"Localhost Web Server" in body)

        status, _, body = request("GET", "/", host="alt.local")
        check("virtual host on shared listener", status == 200 and b"alt.local virtual host" in body)

        status, headers, _ = request("GET", "/old-page")
        check("configured redirect", status == 301 and headers.get("Location") == "/")

        status, _, body = request("GET", "/definitely-missing")
        check("custom 404", status == 404 and b"404" in body)

        status, headers, _ = request("DELETE", "/")
        check("route method restriction", status == 405 and "Allow" in headers)

        payload = b'{"message":"audit"}'
        status, _, body = request("POST", "/api/echo", payload, {"Content-Type": "application/json"})
        check("POST echo", status == 200 and body == payload)

        boundary = "----localhostAuditBoundary"
        file_bytes = bytes(range(256)) * 32
        multipart = (
            f"--{boundary}\r\n"
            'Content-Disposition: form-data; name="file"; filename="integrity.bin"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + file_bytes + f"\r\n--{boundary}--\r\n".encode()
        status, _, _ = request("POST", "/uploads", multipart, {"Content-Type": f"multipart/form-data; boundary={boundary}"})
        check("multipart upload", status == 201)
        status, _, downloaded = request("GET", "/uploads/integrity.bin")
        check(
            "upload/download integrity",
            status == 200 and hashlib.sha256(downloaded).digest() == hashlib.sha256(file_bytes).digest(),
        )
        status, _, _ = request("DELETE", "/uploads/integrity.bin")
        check("DELETE uploaded file", status == 204)
        status, _, _ = request("GET", "/uploads/integrity.bin")
        check("deleted file is gone", status == 404)

        chunked = raw_http(
            b"POST /uploads/chunked.txt HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Content-Type: text/plain\r\n"
            b"Connection: close\r\n\r\n"
            b"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
        )
        check("chunked POST accepted", chunked.startswith(b"HTTP/1.1 201"))
        status, _, body = request("GET", "/uploads/chunked.txt")
        check("chunked body decoded", status == 200 and body == b"Wikipedia")

        status, headers, body = request("GET", "/session?user=Auditor")
        cookie = headers.get("Set-Cookie", "").split(";", 1)[0]
        check("session creates cookie", status == 200 and cookie.startswith("session_id="))
        status, _, body2 = request("GET", "/session", headers={"Cookie": cookie})
        check("session persists", status == 200 and b'"visits":2' in body2)

        status, _, body = request("GET", "/cgi-bin/test.py/audit/path?name=Auditor")
        check("Python CGI GET + PATH_INFO", status == 200 and b"/audit/path" in body)

        cgi_chunked = raw_http(
            b"POST /cgi-bin/test.py/chunked HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Content-Type: text/plain\r\n"
            b"Connection: close\r\n\r\n"
            b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"
        )
        check("Python CGI chunked POST", cgi_chunked.startswith(b"HTTP/1.1 200") and b"hello world" in cgi_chunked)

        if shutil.which("php-cgi"):
            status, _, body = request("GET", "/cgi-bin/test.php/bonus?name=Auditor")
            check("PHP CGI bonus", status == 200 and b"PHP CGI Execution Succeeded" in body)
        else:
            print("[SKIP] PHP CGI bonus (php-cgi not installed)")

        big = b"x" * (2 * 1024 * 1024 + 1)
        status, _, _ = request("POST", "/uploads/too-big.bin", big, {"Content-Type": "application/octet-stream"})
        check("client body limit -> 413", status == 413)

        malformed = raw_http(b"BROKEN REQUEST\r\n\r\n")
        check("malformed request -> 400", malformed.startswith(b"HTTP/1.1 400"))
        status, _, _ = request("GET", "/")
        check("server survives malformed request", status == 200 and proc.poll() is None)

        status, _, body = request("GET", "/uploads/")
        check("autoindex route", status == 200 and b"chunked.txt" in body)

        print("\nAll black-box audit checks passed.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        stderr = proc.stderr.read() if proc.stderr else ""
        if proc.returncode not in (0, -15):
            print(stderr)
        cfg.unlink(missing_ok=True)
        for child in uploads.iterdir():
            if child.name != ".gitkeep" and child.is_file():
                child.unlink()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary if args.binary.is_absolute() else (ROOT / args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"binary not found: {binary}")
    run(binary)


if __name__ == "__main__":
    main()
