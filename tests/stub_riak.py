#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal in-memory emulation of the Riak KV HTTP API for integration tests.

Usage: stub_riak.py <test-binary> [args...]
Starts the server on a free port, runs the test binary with RIAK_TEST_PORT
set, and exits with the binary's exit code.

Three nodes are started (sharing one store, like a cluster); their ports are
in RIAK_TEST_PORTS, and RIAK_TEST_PORT is the first one.

Extra endpoints for assertions:
  GET /_last  -> JSON {"method", "path", "headers", "body"} of the previous request
  GET /_mode?set=ok|503|close  -> make this node answer normally, with 503, or
                                  by closing the connection without a reply
  GET /_hits  -> number of requests this node received (resets it)

Bucket types: "default", "t1" and "multi" (allow_mult, so writes without a
vclock create siblings). Special buckets: "many" (1000 streamed keys),
"siblings" (always 300 with a text body), "unavailable" (503), "truncated"
(cut-off key stream), "notjson" (props are not an object), "nolocation"
(POST without Location).
"""

import itertools
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit, parse_qs

TYPES = {"default": {"allow_mult": False}, "t1": {"allow_mult": False},
         "multi": {"allow_mult": True}}
for _dt in ("counter", "set", "map", "hll", "gset"):
    TYPES[_dt + "s"] = {"allow_mult": True, "datatype": _dt}
DATATYPES = {}      # (type, bucket, key) -> python value
DT_FIELD_TYPES = ("counter", "set", "map", "register", "flag")
STORE = {}          # (type, bucket, key) -> {"vclock": str, "siblings": [obj]}
PROPS = {}          # (type, bucket) -> dict
LAST = {}
LOCK = threading.Lock()
COUNTER = itertools.count(1)
STREAM_BUCKET = "many"
STREAM_KEYS = [f"key-{i}" for i in range(1000)]
BOUNDARY = "StubBoundary7x"
KEPT_HEADERS = ("x-riak-meta-", "x-riak-index-", "link")
SEARCH_INDEXES = {}  # name -> {"name", "schema", "n_val"}
SCHEMAS = {"_yz_default": '<schema name="_yz_default" version="1.5"/>'}


def object_headers(obj):
    headers = [("Content-Type", obj["ctype"]), ("ETag", obj["etag"]),
               ("Last-Modified", "Mon, 26 Oct 2026 10:00:00 GMT")]
    headers += obj["extra"]
    if obj.get("deleted"):
        headers.append(("X-Riak-Deleted", "true"))
    return headers


def index_entries(btype, bucket, index):
    """Sorted (term, key) pairs of one index; values compare as ints for _int."""
    out = []
    with LOCK:
        items = [(k, e) for (t, b, k), e in STORE.items() if t == btype and b == bucket]
    for key, entry in items:
        if index == "$key":
            out.append((key, key))
        elif index == "$bucket":
            out.append((bucket, key))
        else:
            for obj in entry["siblings"]:
                for name, value in obj["extra"]:
                    if name.lower() == "x-riak-index-" + index.lower():
                        out.extend((v.strip(), key) for v in value.split(","))
    as_int = index.endswith("_int")
    return sorted(set(out), key=lambda tk: (int(tk[0]) if as_int else tk[0], tk[1])), as_int


class BadOp(Exception):
    pass


def dt_empty(kind):
    return {"counter": 0, "hll": [], "set": [], "gset": [], "map": {}}[kind]


def dt_apply(kind, value, op, has_context):
    """Apply one update document; raises BadOp or KeyError (precondition)."""
    if kind == "counter":
        if isinstance(op, int) and not isinstance(op, bool):
            return value + op
        if not isinstance(op, dict) or set(op) - {"increment", "decrement"}:
            raise BadOp()
        return value + op.get("increment", 0) - op.get("decrement", 0)
    if kind in ("set", "gset", "hll"):
        allowed = {"add", "add_all"} | ({"remove", "remove_all"} if kind == "set" else set())
        if not isinstance(op, dict) or not op or set(op) - allowed:
            raise BadOp()
        adds = op.get("add_all", []) + ([op["add"]] if "add" in op else [])
        removes = op.get("remove_all", []) + ([op["remove"]] if "remove" in op else [])
        if kind == "hll":
            return value  # cardinality is tracked separately
        members = set(value) | set(adds)
        for r in removes:
            if r not in members and not has_context:
                raise KeyError(r)
            members.discard(r)
        return sorted(members)
    if kind == "map":
        if not isinstance(op, dict) or not op or set(op) - {"update", "remove"}:
            raise BadOp()
        out = dict(value)
        for field in op.get("remove", []):
            if field not in out and not has_context:
                raise KeyError(field)
            out.pop(field, None)
        for field, sub in op.get("update", {}).items():
            name, _, ftype = field.rpartition("_")
            if not name or ftype not in DT_FIELD_TYPES:
                raise BadOp()
            if ftype == "register":
                if not isinstance(sub, str):
                    raise BadOp()
                out[field] = sub
            elif ftype == "flag":
                if sub not in ("enable", "disable"):
                    raise BadOp()
                out[field] = sub == "enable"
            else:
                out[field] = dt_apply(ftype, out.get(field, dt_empty(ftype)), sub, has_context)
        return out
    raise BadOp()


def multipart(parts):
    body = b""
    for part in parts:
        body += f"\r\n--{BOUNDARY}\r\nContent-Type: application/json\r\n\r\n".encode()
        body += json.dumps(part).encode()
    return body + f"\r\n--{BOUNDARY}--\r\n".encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body=b"", headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        for k, v in (headers.items() if isinstance(headers, dict) else headers or []):
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, code, doc):
        return self._send(code, json.dumps(doc), {"Content-Type": "application/json"})

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n) if n else b""
        with LOCK:
            LAST["body"] = data.decode("latin-1")
        return data

    def _route(self):
        """Return (type, parts, query); parts start at "buckets" when typed."""
        url = urlsplit(self.path)
        parts = [unquote(p) for p in url.path.strip("/").split("/")]
        query = parse_qs(url.query)
        if not parts[0].startswith("_"):
            self.server.hits += 1
        if parts[0] != "_last" and parts[0].startswith("_"):
            return "default", parts, query
        if parts != ["_last"]:
            with LOCK:
                LAST.clear()
                LAST.update(method=self.command, path=self.path,
                            headers={k: v for k, v in self.headers.items()})
        btype = "default"
        if len(parts) >= 3 and parts[0] == "types" and parts[2] == "buckets":
            btype, parts = parts[1], parts[2:]
        return btype, parts, query

    def _failing(self):
        """Apply this node's failure mode; True when the request was handled."""
        mode = self.server.mode
        if mode == "503":
            self._body()
            self._send(503, "<html>Service Unavailable</html>")
            return True
        if mode == "close":
            self.close_connection = True
            return True
        return False

    # ---- GET ----------------------------------------------------------
    def do_GET(self):
        btype, parts, query = self._route()
        if parts == ["_last"]:
            with LOCK:
                return self._json(200, LAST)
        if parts == ["_mode"]:
            self.server.mode = query.get("set", ["ok"])[0]
            return self._send(204)
        if parts == ["_hits"]:
            hits, self.server.hits = self.server.hits, 0
            return self._json(200, {"hits": hits})
        if self._failing():
            return None
        if parts == ["ping"]:
            return self._send(200, "OK", {"Content-Type": "text/plain"})
        if parts == ["stats"]:
            return self._json(200, {"vnode_gets": 0, "node_gets": 0, "ring_num_partitions": 64})
        if len(parts) == 3 and parts[0] == "types" and parts[2] == "props":
            if parts[1] not in TYPES:
                return self._send(404, "no such bucket type\n")
            return self._json(200, {"props": dict(TYPES[parts[1]], n_val=3)})
        if parts[:1] == ["search"]:
            return self._search_get(parts[1:], query)
        if btype not in TYPES:
            return self._send(404, "no such bucket type\n")
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "datatypes":
            return self._dt_get(btype, parts[1], parts[3], query)
        if len(parts) >= 5 and parts[0] == "buckets" and parts[2] == "index":
            return self._index_query(btype, parts[1], parts[3], parts[4:], query)
        if parts == ["buckets"] and query.get("buckets") == ["true"]:
            with LOCK:
                names = {b for t, b, _ in STORE if t == btype}
            if btype == "default":
                names.add(STREAM_BUCKET)
            return self._json(200, {"buckets": sorted(names)})
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "keys":
            return self._stream_keys(btype, parts[1], query)
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            if parts[1] == "unavailable":
                return self._send(503, "service unavailable\n")
            if parts[1] == "notjson":
                return self._send(200, "[1, 2]", {"Content-Type": "application/json"})
            props = PROPS.get((btype, parts[1]), dict(TYPES[btype], n_val=3))
            return self._json(200, {"props": props})
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            return self._get_object(btype, parts[1], parts[3])
        return self._send(400, "bad request\n")

    def _get_object(self, btype, bucket, key):
        if bucket == "unavailable":
            return self._send(503, "service unavailable\n")
        if bucket == "siblings":
            return self._send(300, "Siblings:\n1\n2\n")
        with LOCK:
            entry = STORE.get((btype, bucket, key))
            entry = json.loads(json.dumps(entry, default=list)) if entry else None
        if entry is None:
            return self._send(404, "not found\n")
        sibs = entry["siblings"]
        if len(sibs) > 1:
            if "multipart/mixed" not in (self.headers.get("Accept") or ""):
                return self._send(300, "Siblings:\n" + "".join(s["etag"] + "\n" for s in sibs))
            body = b""
            for s in sibs:
                body += f"\r\n--{BOUNDARY}\r\n".encode()
                body += "".join(f"{k}: {v}\r\n" for k, v in object_headers(s)).encode()
                body += b"\r\n" + s["body"].encode("latin-1")
            body += f"\r\n--{BOUNDARY}--\r\n".encode()
            return self._send(300, body, [
                ("Content-Type", f"multipart/mixed; boundary={BOUNDARY}"),
                ("X-Riak-Vclock", entry["vclock"])])
        obj = sibs[0]
        if self.headers.get("If-None-Match") == obj["etag"]:
            return self._send(304, "", [("ETag", obj["etag"])])
        return self._send(200, obj["body"].encode("latin-1"),
                          object_headers(obj) + [("X-Riak-Vclock", entry["vclock"])])

    def _dt_doc(self, btype, key3, query):
        kind = TYPES[btype]["datatype"]
        value = DATATYPES[key3]
        doc = {"type": kind, "value": len(value) if kind == "hll" else value}
        if kind != "counter" and kind != "hll" and query.get("include_context") != ["false"]:
            doc["context"] = f"ctx-{next(COUNTER)}"
        return doc

    def _dt_get(self, btype, bucket, key, query):
        kind = TYPES[btype].get("datatype")
        if kind is None:
            return self._send(404, "not a datatype bucket type\n")
        with LOCK:
            if (btype, bucket, key) not in DATATYPES:
                return self._json(404, {"type": kind, "error": "notfound"})
            return self._json(200, self._dt_doc(btype, (btype, bucket, key), query))

    def _dt_update(self, btype, bucket, key, query):
        kind = TYPES.get(btype, {}).get("datatype")
        body = self._body()
        if kind is None:
            return self._send(404, "not a datatype bucket type\n")
        try:
            op = json.loads(body)
        except ValueError:
            return self._send(400, "bad json\n")
        has_context = isinstance(op, dict) and "context" in op
        if isinstance(op, dict):
            op = {k: v for k, v in op.items() if k != "context"}
        created = key is None
        if created:
            key = f"gen{next(COUNTER)}"
        k3 = (btype, bucket, key)
        with LOCK:
            current = DATATYPES.get(k3, dt_empty(kind))
            try:
                if kind == "hll":
                    dt_apply("hll", 0, op, has_context)
                    seen = set(current) | set(op.get("add_all", []) + ([op["add"]] if "add" in op else []))
                    DATATYPES[k3] = sorted(seen)
                else:
                    DATATYPES[k3] = dt_apply(kind, current, op, has_context)
            except BadOp:
                return self._send(400, f"Invalid operation on datatype '{kind}': {body.decode()}")
            except KeyError as missing:
                return self._send(500, f"Error:\n{{precondition,{{not_present,<<{missing}>>}}}}\n")
            doc = self._dt_doc(btype, k3, query)
        if created:
            return self._send(201, "", {"Location": f"/types/{btype}/buckets/{bucket}/keys/{key}"})
        if query.get("returnbody") == ["true"]:
            return self._json(200, doc)
        return self._send(204)

    def _index_query(self, btype, bucket, index, values, query):
        if bucket == "unavailable":
            return self._send(500, "{error,{indexes_not_supported,riak_kv_bitcask_backend}}")
        entries, as_int = index_entries(btype, bucket, index)
        conv = int if as_int else str
        try:
            if len(values) == 1:
                hits = [tk for tk in entries if conv(tk[0]) == conv(values[0])]
            elif len(values) == 2:
                lo, hi = conv(values[0]), conv(values[1])
                hits = [tk for tk in entries if lo <= conv(tk[0]) <= hi]
            else:
                return self._send(400, "bad index query\n")
        except ValueError:
            return self._send(400, "bad integer\n")
        terms = query.get("return_terms") == ["true"] and len(values) == 2
        start = int(query["continuation"][0][1:]) if "continuation" in query else 0
        limit = int(query.get("max_results", ["0"])[0])
        page = hits[start:start + limit] if limit else hits[start:]
        cont = f"c{start + limit}" if limit and start + limit < len(hits) else None
        def doc(chunk):
            return {"results": [{t: k} for t, k in chunk]} if terms else {"keys": [k for _, k in chunk]}
        if query.get("stream") == ["true"]:
            parts = [doc(page[i:i + 2]) for i in range(0, len(page), 2)]
            if cont:
                parts.append({"continuation": cont})
            return self._send(200, multipart(parts),
                              {"Content-Type": f"multipart/mixed;boundary={BOUNDARY}"})
        result = doc(page)
        if cont:
            result["continuation"] = cont
        return self._json(200, result)

    def _search_get(self, parts, query):
        if parts == ["index"]:
            return self._json(200, sorted(SEARCH_INDEXES.values(), key=lambda i: i["name"]))
        if len(parts) == 2 and parts[0] == "index":
            idx = SEARCH_INDEXES.get(parts[1])
            return self._json(200, idx) if idx else self._send(404, "not found\n")
        if len(parts) == 2 and parts[0] == "schema":
            xml = SCHEMAS.get(parts[1])
            return self._send(200, xml, {"Content-Type": "application/xml"}) if xml \
                else self._send(404, "not found\n")
        if len(parts) == 2 and parts[0] == "query":
            return self._search_query(parts[1], query)
        return self._send(400, "bad request\n")

    def _search_query(self, index, query):
        if index not in SEARCH_INDEXES:
            return self._send(404, "No index found\n")
        if query.get("wt") != ["json"] or "q" not in query:
            return self._send(400, "expected wt=json and q\n")
        field, _, value = query["q"][0].partition(":")
        docs = []
        with LOCK:
            buckets = [tb for tb, props in PROPS.items() if props.get("search_index") == index]
            for (t, b, k), entry in STORE.items():
                if (t, b) not in buckets:
                    continue
                try:
                    data = json.loads(entry["siblings"][0]["body"])
                except ValueError:
                    continue
                if (field, value) == ("*", "*") or str(data.get(field)) == value:
                    docs.append(dict(data, _yz_rk=k, _yz_rb=b, _yz_rt=t, score=1.0))
        if "sort" in query:
            name, _, order = query["sort"][0].partition(" ")
            docs.sort(key=lambda d: d.get(name, 0), reverse=order == "desc")
        start = int(query.get("start", ["0"])[0])
        rows = int(query.get("rows", ["10"])[0])
        return self._json(200, {"responseHeader": {"status": 0},
                                "response": {"numFound": len(docs), "start": start,
                                             "maxScore": 1.0 if docs else 0.0,
                                             "docs": docs[start:start + rows]}})

    def _mapred(self, query):
        try:
            job = json.loads(self._body())
            inputs = [tuple(i) for i in job["inputs"]]
            phases = job["query"]
        except (ValueError, KeyError, TypeError):
            return self._json(400, {"error": "bad job"})
        results = []
        values = []
        with LOCK:
            for b, k in inputs:
                entry = STORE.get(("default", b, k))
                if entry:
                    values.append(entry["siblings"][0]["body"])
        data = values
        for n, phase in enumerate(phases):
            kind = "map" if "map" in phase else "reduce"
            if kind == "reduce":
                data = [len(data)]
            results.append((n, data))
        if query.get("chunked") == ["true"]:
            parts = []
            for n, d in results:
                parts.extend({"phase": n, "data": [v]} for v in d) if n == 0 else \
                    parts.append({"phase": n, "data": d})
            return self._send(200, multipart(parts),
                              {"Content-Type": f"multipart/mixed;boundary={BOUNDARY}"})
        return self._json(200, results[-1][1] if results else [])

    def _stream_keys(self, btype, bucket, query):
        if query.get("keys") != ["stream"]:
            return self._send(400, "expected keys=stream\n")
        if bucket == "unavailable":
            return self._send(503, "service unavailable\n")
        if bucket == "truncated":
            return self._send(200, '{"keys":["a"]}{"keys":["b"',
                              {"Content-Type": "application/json"})
        with LOCK:
            keys = STREAM_KEYS if bucket == STREAM_BUCKET else \
                sorted(k for t, b, k in STORE if t == btype and b == bucket)
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

    # ---- writes -------------------------------------------------------
    def _new_object(self, body):
        return {
            "body": body.decode("latin-1"),
            "ctype": self.headers.get("Content-Type", "application/octet-stream"),
            "etag": f'"e{next(COUNTER)}"',
            "extra": [(k, v) for k, v in self.headers.items()
                      if k.lower().startswith(KEPT_HEADERS)],
        }

    def _store(self, btype, bucket, key, query):
        """Returns True when a response was already sent."""
        body = self._body()
        vclock = self.headers.get("X-Riak-Vclock")
        with LOCK:
            entry = STORE.get((btype, bucket, key))
            current = entry["siblings"] if entry else []
            inm = self.headers.get("If-None-Match")
            im = self.headers.get("If-Match")
            failed = (vclock == "stale" or (inm == "*" and current) or
                      (inm and inm != "*" and any(s["etag"] == inm for s in current)) or
                      (im and not any(s["etag"] == im for s in current)))
            if not failed:
                obj = self._new_object(body)
                if TYPES[btype]["allow_mult"] and current and vclock != (entry or {}).get("vclock"):
                    siblings = current + [obj]
                else:
                    siblings = [obj]
                entry = {"vclock": f"vc-{next(COUNTER)}", "siblings": siblings}
                STORE[(btype, bucket, key)] = entry
        if failed:
            self._send(412, "precondition failed\n")
            return True
        if query.get("returnbody") == ["true"]:
            self._send(200, body, object_headers(obj) + [("X-Riak-Vclock", entry["vclock"])])
            return True
        return False

    def do_PUT(self):
        btype, parts, query = self._route()
        if self._failing():
            return None
        if len(parts) == 3 and parts[:2] == ["search", "index"]:
            spec = json.loads(self._body() or b"{}")
            SEARCH_INDEXES[parts[2]] = {"name": parts[2],
                                        "schema": spec.get("schema", "_yz_default"),
                                        "n_val": spec.get("n_val", 3)}
            return self._send(204)
        if len(parts) == 3 and parts[:2] == ["search", "schema"]:
            SCHEMAS[parts[2]] = self._body().decode()
            return self._send(204)
        if btype not in TYPES:
            self._body()
            return self._send(404, "no such bucket type\n")
        if parts[1:] == ["unavailable", "props"]:
            self._body()
            return self._send(503, "service unavailable\n")
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            PROPS[(btype, parts[1])] = json.loads(self._body() or b"{}").get("props", {})
            return self._send(204)
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            if not self._store(btype, parts[1], parts[3], query):
                self._send(204)
            return None
        self._body()
        return self._send(400, "bad request\n")

    def do_POST(self):
        btype, parts, query = self._route()
        if self._failing():
            return None
        if parts == ["mapred"]:
            return self._mapred(query)
        if len(parts) in (3, 4) and parts[0] == "buckets" and parts[2] == "datatypes":
            return self._dt_update(btype, parts[1], parts[3] if len(parts) == 4 else None, query)
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "keys":
            body = self._body()
            if parts[1] == "nolocation":
                return self._send(201)
            key = "auto key"
            prefix = "" if btype == "default" else f"/types/{quote(btype, safe='')}"
            location = f"{prefix}/buckets/{quote(parts[1], safe='')}/keys/{quote(key, safe='')}"
            with LOCK:
                STORE[(btype, parts[1], key)] = {"vclock": "vc-auto",
                                                 "siblings": [self._new_object(body)]}
            return self._send(201, "", {"Location": location})
        self._body()
        return self._send(400, "bad request\n")

    def do_DELETE(self):
        btype, parts, query = self._route()
        if self._failing():
            return None
        if len(parts) == 3 and parts[:2] == ["search", "index"]:
            existed = SEARCH_INDEXES.pop(parts[2], None) is not None
            return self._send(204 if existed else 404)
        if parts[1:] == ["unavailable", "props"]:
            return self._send(503, "service unavailable\n")
        if len(parts) == 3 and parts[0] == "buckets" and parts[2] == "props":
            PROPS.pop((btype, parts[1]), None)
            return self._send(204)
        if len(parts) == 4 and parts[0] == "buckets" and parts[2] == "keys":
            with LOCK:
                existed = STORE.pop((btype, parts[1], parts[3]), None) is not None
            return self._send(204 if existed else 404, "" if existed else "not found\n")
        return self._send(400, "bad request\n")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    mode = "ok"
    hits = 0

    def handle_error(self, request, client_address):
        # clients aborting a transfer (e.g. the body size limit) are expected
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    servers = [Server(("127.0.0.1", 0), Handler) for _ in range(3)]
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    ports = [str(server.server_address[1]) for server in servers]
    env = dict(os.environ, RIAK_TEST_PORT=ports[0], RIAK_TEST_PORTS=",".join(ports))
    try:
        return subprocess.call(sys.argv[1:], env=env)
    finally:
        for server in servers:
            server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
