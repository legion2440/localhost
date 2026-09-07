#!/usr/bin/env python3
"""Small local availability check when siege is unavailable."""

from concurrent.futures import ThreadPoolExecutor
import http.client
import sys
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
REQUESTS = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
CONCURRENCY = int(sys.argv[3]) if len(sys.argv) > 3 else 50


def one(_):
    try:
        c = http.client.HTTPConnection(HOST, PORT, timeout=5)
        c.request("GET", "/", headers={"Host": "localhost"})
        r = c.getresponse()
        r.read()
        ok = r.status == 200
        c.close()
        return ok
    except Exception:
        return False


start = time.time()
with ThreadPoolExecutor(max_workers=CONCURRENCY) as pool:
    results = list(pool.map(one, range(REQUESTS)))
ok = sum(results)
availability = ok * 100 / REQUESTS
print(f"requests={REQUESTS} success={ok} availability={availability:.2f}% elapsed={time.time()-start:.2f}s")
raise SystemExit(0 if availability >= 99.5 else 1)
