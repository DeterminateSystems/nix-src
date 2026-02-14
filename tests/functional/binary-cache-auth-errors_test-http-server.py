#!/usr/bin/env python3
"""
Test HTTP server for binary cache authentication error testing.

This server serves files from a directory but can be configured to return
specific HTTP status codes (401, 403, 404) for NAR files while still serving
metadata files (.narinfo, nix-cache-info) normally.

Environment variables:
- CACHE_DIR: Directory to serve files from
- RETURN_CODE: HTTP status code to return for NAR files (default: 200)
- TEST_SERVER_PORT: Port to bind to (0 = random port, default)
"""

import http.server
import socketserver
import os
import sys
from urllib.parse import urlparse, parse_qs

PORT = int(os.environ.get('TEST_SERVER_PORT', 0))
CACHE_DIR = os.environ.get('CACHE_DIR', '')
RETURN_CODE = int(os.environ.get('RETURN_CODE', 200))

class TestHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        # Remove leading slash
        path = self.path.lstrip('/')
        file_path = os.path.join(CACHE_DIR, path)

        # Check if we should return an error for NAR files
        # Always serve nix-cache-info and .narinfo files normally
        if RETURN_CODE != 200 and (path.startswith('nar/') or path.endswith('.nar') or path.endswith('.nar.xz')):
            self.send_response(RETURN_CODE)
            if RETURN_CODE == 401:
                self.send_header('WWW-Authenticate', 'Bearer realm="test"')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return

        # Otherwise, serve files from the cache directory
        if os.path.isfile(file_path):
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            with open(file_path, 'rb') as f:
                content = f.read()
                self.send_header('Content-Length', str(len(content)))
                self.end_headers()
                self.wfile.write(content)
        else:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()

    def log_message(self, format, *args):
        # Suppress logging
        pass

# Bind to a random port if PORT is 0
with socketserver.TCPServer(("127.0.0.1", PORT), TestHandler) as httpd:
    # Print the actual port we're listening on
    print(httpd.server_address[1], flush=True)
    httpd.serve_forever()
