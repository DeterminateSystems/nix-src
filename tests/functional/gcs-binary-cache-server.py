"""Local GCS JSON API and ADC endpoints; never contacts Google or uses real credentials."""

import base64
import datetime
import hashlib
import http.server
import json
import pathlib
import re
import sys
import urllib.parse


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return base64.b64encode((crc ^ 0xFFFFFFFF).to_bytes(4, "big")).decode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, data=b"", headers=None):
        if isinstance(data, dict):
            data = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", "application/json")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def handle_request(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        auth = self.headers.get("Authorization")
        with (self.server.root / "requests").open("a") as log:
            log.write(json.dumps({"method": self.command, "path": url.path, "auth": auth, "size": len(body)}) + "\n")

        if url.path.startswith("/computeMetadata/v1/"):
            assert self.headers.get("Metadata-Flavor") == "Google"
            if url.path.endswith("/token"):
                return self.reply(200, {"access_token": "metadata-token", "expires_in": 0, "token_type": "Bearer"},
                                  {"Metadata-Flavor": "Google"})
            if url.path.endswith("/email"):
                return self.reply(200, b"test@example.iam.gserviceaccount.com", {"Metadata-Flavor": "Google"})
            if url.path.endswith("/universe-domain"):
                return self.reply(200, b"googleapis.com", {"Metadata-Flavor": "Google"})
            return self.reply(200, {"email": "test@example.iam.gserviceaccount.com", "scopes": ["https://www.googleapis.com/auth/cloud-platform"]},
                              {"Metadata-Flavor": "Google"})

        if url.path == "/token":
            fields = urllib.parse.parse_qs(body.decode())
            assert fields["subject_token"] == ["test-subject-token"]
            assert fields["grant_type"] == ["urn:ietf:params:oauth:grant-type:token-exchange"]
            return self.reply(200, {"access_token": "federated-token", "expires_in": 0,
                                   "token_type": "Bearer", "issued_token_type": "urn:ietf:params:oauth:token-type:access_token"})

        if url.path == "/impersonate":
            assert auth == "Bearer federated-token"
            # Expired tokens force the real ADC client to refresh within one process.
            expiry = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            return self.reply(200, {"accessToken": "impersonated-token", "expireTime": expiry})

        if auth not in ("Bearer metadata-token", "Bearer federated-token", "Bearer impersonated-token"):
            return self.reply(401, {"error": {"code": 401, "message": "missing ADC token"}})

        if "/b/denied/" in url.path:
            return self.reply(403, {"error": {"code": 403, "message": "permission denied by test"}})

        if self.command == "GET" and "/o/" in url.path:
            bucket, name = url.path.split("/b/", 1)[1].split("/o/", 1)
            key = (bucket, urllib.parse.unquote(name))
            if key not in self.server.objects:
                return self.reply(404, {"error": {"code": 404, "message": "object not found"}})
            data, metadata = self.server.objects[key]
            if query.get("alt") != ["media"]:
                return self.reply(200, metadata)
            checksum = metadata["md5Hash"]
            if (self.server.root / "corrupt").exists() and "/nar/" in key[1]:
                checksum = base64.b64encode(b"x" * 16).decode()
            headers = {"x-goog-hash": f"md5={checksum},crc32c={metadata['crc32c']}", "x-goog-generation": "1"}
            if self.headers.get("Range"):
                start = int(self.headers["Range"].split("=")[1].split("-")[0])
                headers["Content-Range"] = f"bytes {start}-{len(data) - 1}/{len(data)}"
                return self.reply(206, data[start:], headers)
            return self.reply(200, data, headers)

        if self.command == "POST" and url.path.startswith("/upload/storage/v1/b/"):
            assert query["uploadType"] == ["resumable"]
            metadata = json.loads(body or b"{}")
            name = query.get("name", [metadata.get("name")])[0]
            bucket = url.path.split("/b/", 1)[1].split("/", 1)[0]
            session = str(len(self.server.uploads))
            self.server.uploads[session] = (bucket, name, bytearray(), metadata)
            return self.reply(200, {}, {"Location": f"http://127.0.0.1:{self.server.server_port}/session/{session}"})

        if self.command == "PUT" and url.path.startswith("/session/"):
            bucket, name, data, metadata = self.server.uploads[url.path.rsplit("/", 1)[1]]
            content_range = self.headers["Content-Range"]
            if content_range.startswith("bytes */"):
                # Upload status query after a retryable failure.
                if content_range == "bytes */*":
                    headers = {"Range": f"bytes=0-{len(data) - 1}"} if data else {}
                    return self.reply(308, headers=headers)
                total = int(content_range.split("/")[1])
                assert total == len(data)
            else:
                match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+|\*)", content_range)
                assert match, content_range
                start, end, total = match.groups()
                assert int(start) == len(data)
                assert int(end) - int(start) + 1 == len(body)
                # Make the SDK retry a large chunk, including querying the session.
                if len(body) > 1024 * 1024 and not self.server.retried:
                    self.server.retried = True
                    return self.reply(503, {"error": {"code": 503, "message": "retry this chunk"}})
                data.extend(body)
                if total == "*":
                    return self.reply(308, headers={"Range": f"bytes=0-{len(data) - 1}"})
                assert len(data) == int(total)
            metadata.update({"name": name, "bucket": bucket, "size": str(len(data)), "generation": "1",
                             "md5Hash": base64.b64encode(hashlib.md5(data).digest()).decode(), "crc32c": crc32c(data)})
            self.server.objects[(bucket, name)] = (bytes(data), metadata)
            return self.reply(200, metadata)

        return self.reply(400, {"error": {"code": 400, "message": f"unexpected request: {self.command} {self.path}"}})

    do_GET = handle_request
    do_POST = handle_request
    do_PUT = handle_request


def main():
    root = pathlib.Path(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.root = root
    server.objects = {}
    server.uploads = {}
    server.retried = False
    (root / "port").write_text(str(server.server_port))
    server.serve_forever()


if __name__ == "__main__":
    main()
