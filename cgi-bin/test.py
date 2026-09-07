#!/usr/bin/env python3
import sys
import os
import urllib.parse
import html
from datetime import datetime

# CGI Output Header
print("Content-Type: text/html; charset=utf-8")
print()

method = os.environ.get("REQUEST_METHOD", "GET")
query_string = os.environ.get("QUERY_STRING", "")
path_info = os.environ.get("PATH_INFO", "")
content_length = os.environ.get("CONTENT_LENGTH", "0")
cookie = os.environ.get("HTTP_COOKIE", "None")

# Read Body until EOF (as required by 01-edu subject)
body_data = ""
if method == "POST":
    try:
        # Read until EOF
        body_data = sys.stdin.read()
    except Exception as e:
        body_data = f"Error reading body: {e}"

print("<!DOCTYPE html>")
print("<html><head><title>Python CGI Response</title><style>")
print("body { font-family: monospace; background: #0f172a; color: #38bdf8; padding: 20px; }")
print(".block { background: #1e293b; padding: 15px; border-radius: 8px; margin-bottom: 12px; }")
print("h2 { color: #f8fafc; }")
print("</style></head><body>")

print("<h2>Python CGI Execution Success!</h2>")
print(f"<div class='block'><strong>Server Time:</strong> {datetime.now().isoformat()}</div>")
print(f"<div class='block'><strong>REQUEST_METHOD:</strong> {html.escape(method)}</div>")
print(f"<div class='block'><strong>PATH_INFO:</strong> {html.escape(path_info)}</div>")
print(f"<div class='block'><strong>QUERY_STRING:</strong> {html.escape(query_string)}</div>")
print(f"<div class='block'><strong>HTTP_COOKIE:</strong> {html.escape(cookie)}</div>")

if body_data:
    print(f"<div class='block'><strong>POST Body Read (EOF):</strong><br>{html.escape(body_data)}</div>")

print("<div class='block'><strong>Environment Variables:</strong><ul>")
for key, val in sorted(os.environ.items()):
    if key.startswith("HTTP_") or key in ["REQUEST_METHOD", "QUERY_STRING", "PATH_INFO", "CONTENT_LENGTH", "SERVER_PORT"]:
        print(f"<li><strong>{html.escape(key)}:</strong> {html.escape(val)}</li>")
print("</ul></div>")

print("</body></html>")
