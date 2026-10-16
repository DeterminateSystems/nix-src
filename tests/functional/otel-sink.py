#!/usr/bin/env python3
"""A minimal OTLP/HTTP collector for testing: saves every POST body it
receives, unmodified, to a numbered file in the given directory, along
with the request headers. Listens on an ephemeral port on 127.0.0.1
and writes the port number to `<dir>/port` once it's ready."""

import http.server
import json
import os
import sys

out_dir = sys.argv[1]
count = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        global count
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        with open(os.path.join(out_dir, f"{count}.body"), "wb") as f:
            f.write(body)
        with open(os.path.join(out_dir, f"{count}.headers"), "w") as f:
            json.dump({k.lower(): v for k, v in self.headers.items()}, f)
        count += 1
        self.send_response(200)
        self.end_headers()

    def log_message(self, *args):
        pass


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)

# Write the port atomically, so the test never sees a partial file.
port_file = os.path.join(out_dir, "port")
with open(port_file + ".tmp", "w") as f:
    f.write(str(server.server_address[1]))
os.rename(port_file + ".tmp", port_file)

server.serve_forever()
