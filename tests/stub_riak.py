#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal in-memory emulation of the Riak KV HTTP API for integration tests.

Usage: stub_riak.py <test-binary> [args...]
Starts the server on a free port, runs the test binary with RIAK_TEST_PORT
set, and exits with the binary's exit code.

Extra endpoints for assertions:
  GET /_last  -> JSON {"method", "path", "headers"} of the previous request
"""

import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit, parse_qs

STORE = {}          # (bucket, key) -> dict(body, ctype, vclock)
PROPS = {}          # bucket -> dict
LAST = {}
LOCK = threading.Lock()
STREAM_BUCKET = "many"
STREAM_KEYS = [f"key-{i}" for i in range(1000)]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body=b"", headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _route(self):
        url = urlsplit(self.path)
        raw_parts = url.path.strip("/").split("/")
        parts = [unquote(p) for p in raw_parts]
        query = parse_qs(url.query)
        if parts != ["_last"]:
            with LOCK:
                LAST.clear()
                LAST.update(method=self.command, path=self.path,
                            headers={k: v for k, v in self.headers.items()})
        return parts, query

    def do_GET(self):
        parts, query = self._route()
        if parts == ["_last"]:
            with LOCK:
                return self._send(200, json.dumps(LAST),
                                  {"Content-Type": "application/json"})
        if parts == ["ping"]:
            return self._send(200, "OK", {"Content-Type": "text/plain"})
        if parts == ["buckets"] and query.get("buckets") == ["true"]:
            with LOCK:
                names = sorted({b for b, _ in STORE} | {STREAM_BUCKET})
            return self._send(200, json.dumps({"buckets": names}),
                              {"Content-Type": "application/json"})
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "keys":
            return self._stream_keys(parts[1], query)
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            if parts[1] == "unavailable":
                return self._send(503, "service unavailable\n")
            if parts[1] == "notjson":
                return self._send(200, "[1, 2]", {"Content-Type": "application/json"})
            props = PROPS.get(parts[1], {"n_val": 3, "allow_mult": False})
            return self._send(200, json.dumps({"props": props}),
                              {"Content-Type": "application/json"})
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            bucket, key = parts[1], parts[3]
            if bucket == "unavailable":
                return self._send(503, "service unavailable\n")
            if bucket == "siblings":
                return self._send(300, "Siblings:\n1\n2\n")
            with LOCK:
                obj = STORE.get((bucket, key))
            if obj is None:
                return self._send(404, "not found\n")
            return self._send(200, obj["body"], {
                "Content-Type": obj["ctype"],
                "X-Riak-Vclock": obj["vclock"],
                "ETag": '"etag-1"',
                "Last-Modified": "Mon, 26 Oct 2026 10:00:00 GMT",
            })
        return self._send(400, "bad request\n")

    def _stream_keys(self, bucket, query):
        if query.get("keys") != ["stream"]:
            return self._send(400, "expected keys=stream\n")
        if bucket == "unavailable":
            return self._send(503, "service unavailable\n")
        if bucket == "truncated":
            return self._send(200, '{"keys":["a"]}{"keys":["b"',
                              {"Content-Type": "application/json"})
        keys = STREAM_KEYS if bucket == STREAM_BUCKET else \
            sorted(k for b, k in STORE if b == bucket)
        payload = b"".join(json.dumps({"keys": keys[i:i + 7]}).encode()
                           for i in range(0, len(keys), 7))
        payload = b'{"keys":[]}' + payload + b'{"keys":[]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        # odd chunk size so JSON objects are split across chunks
        for i in range(0, len(payload), 333):
            chunk = payload[i:i + 333]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
        self.wfile.write(b"0\r\n\r\n")

    def _store(self, bucket, key, query):
        body = self._body()
        vclock = self.headers.get("X-Riak-Vclock")
        if vclock == "stale":
            self._send(412, "precondition failed\n")
            return True
        with LOCK:
            STORE[(bucket, key)] = {
                "body": body,
                "ctype": self.headers.get("Content-Type", "application/octet-stream"),
                "vclock": "vc-" + str(len(STORE) + 1),
            }
            obj = STORE[(bucket, key)]
        if query.get("returnbody") == ["true"]:
            self._send(200, body, {"Content-Type": obj["ctype"],
                                   "X-Riak-Vclock": obj["vclock"]})
            return True
        return False

    def do_PUT(self):
        parts, query = self._route()
        if parts[1:] == ["unavailable", "props"]:
            self._body()
            return self._send(503, "service unavailable\n")
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            PROPS[parts[1]] = json.loads(self._body() or b"{}").get("props", {})
            return self._send(204)
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            if not self._store(parts[1], parts[3], query):
                self._send(204)
            return None
        return self._send(400, "bad request\n")

    def do_POST(self):
        parts, query = self._route()
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "keys":
            if parts[1] == "nolocation":
                self._body()
                return self._send(201)
            key = "auto key"
            location = f"/buckets/{quote(parts[1], safe='')}/keys/{quote(key, safe='')}"
            body = self._body()
            with LOCK:
                STORE[(parts[1], key)] = {"body": body, "vclock": "vc-auto",
                                          "ctype": self.headers.get("Content-Type", "")}
            return self._send(201, "", {"Location": location})
        return self._send(400, "bad request\n")

    def do_DELETE(self):
        parts, query = self._route()
        if parts[1:] == ["unavailable", "props"]:
            return self._send(503, "service unavailable\n")
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            PROPS.pop(parts[1], None)
            return self._send(204)
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            with LOCK:
                existed = STORE.pop((parts[1], parts[3]), None) is not None
            return self._send(204 if existed else 404, "" if existed else "not found\n")
        return self._send(400, "bad request\n")


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, RIAK_TEST_PORT=str(server.server_address[1]))
    try:
        return subprocess.call(sys.argv[1:], env=env)
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
