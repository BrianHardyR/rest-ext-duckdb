#!/usr/bin/env python3
"""Local HTTP fixture for the rest_ext call-diagnostics tests.

    python3 fixture_server.py [--port N]     # prints "PORT=<n>" once it is listening

Every route that counts attempts is keyed by an id in the path, so tests do not share state;
GET /_hits?path=/flaky/abc returns how many requests reached that path, GET /_reset forgets all.
"""
import argparse
import json
import sys
import threading
import time
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

lock = threading.Lock()
hits = {}


def count(key):
    with lock:
        hits[key] = hits.get(key, 0) + 1
        return hits[key]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_json(self, value, status=200, headers=None):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def base(self):
        return "http://%s" % self.headers["Host"]

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        path = url.path
        parts = [p for p in path.split("/") if p]
        if path == "/_reset":
            with lock:
                hits.clear()
            return self.send_json({"ok": True})
        if path == "/_hits":
            with lock:
                return self.send_json({"hits": hits.get(q["path"], 0)})
        n = count(path)

        if path in ("/ok", "/offorigin"):
            return self.send_json([{"id": 1}, {"id": 2}])

        # /flaky/<id>?fails=N&status=429&retry_after=1|date|none : N failures, then 200.
        if parts[:1] == ["flaky"]:
            if n <= int(q.get("fails", "1")):
                extra = {"X-RateLimit-Remaining": "0", "X-RateLimit-Limit": "100", "Set-Cookie": "session=SECRET"}
                ra = q.get("retry_after", "none")
                if ra == "date":
                    extra["Retry-After"] = formatdate(time.time() + 2, usegmt=True)
                elif ra != "none":
                    extra["Retry-After"] = ra
                return self.send_json({"error": "slow down"}, int(q.get("status", "429")), extra)
            return self.send_json([{"id": n}], headers={"X-RateLimit-Remaining": "99"})

        # /status/<code>/<id> : always that status.
        if parts[:1] == ["status"]:
            return self.send_json({"error": "status " + parts[1]}, int(parts[1]))

        # /dropped/<id>?fails=N : closes the connection without answering N times, then 200.
        if parts[:1] == ["dropped"]:
            if n <= int(q.get("fails", "1")):
                self.close_connection = True
                self.connection.close()
                return
            return self.send_json([{"id": n}])

        # /slow/<id>?fails=N&ms=M : stalls M ms before answering the first N times, then 200.
        if parts[:1] == ["slow"]:
            if n <= int(q.get("fails", "1000")):
                time.sleep(int(q.get("ms", "1500")) / 1000)
            return self.send_json([{"id": n}])

        # /slowbody : sends the headers and half the body, then stalls.
        if path == "/slowbody":
            body = json.dumps([{"id": i} for i in range(50)]).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body[: len(body) // 2])
            self.wfile.flush()
            time.sleep(2)
            return

        # /big?n=N : N rows, with Content-Length; /bigchunked?n=N : the same, chunked.
        if path in ("/big", "/bigchunked"):
            rows = [{"id": i, "pad": "x" * 40} for i in range(int(q.get("n", "1000")))]
            if path == "/big":
                return self.send_json(rows)
            body = json.dumps(rows).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 1000):
                chunk = body[i : i + 1000]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
            self.wfile.write(b"0\r\n\r\n")
            return

        if path == "/ratelimit":
            return self.send_json([{"id": 1}], headers={
                "X-RateLimit-Remaining": "7", "X-RateLimit-Limit": "100", "RateLimit-Reset": "30",
                "x-ms-ratelimit-remaining-subscription-reads": "11999", "X-Custom-Quota": "5",
                "Set-Cookie": "session=SECRET"})

        if path == "/echo":
            return self.send_json([{"user_agent": self.headers.get("User-Agent"),
                                    "x_api_key": self.headers.get("X-Api-Key"),
                                    "query": url.query}])

        # /reflect : a 400 whose body echoes the request's credentials back, as some APIs do.
        if path == "/reflect":
            return self.send_json({"error": "bad request %s key=%s" % (url.query, self.headers.get("X-Api-Key"))}, 400)

        if path == "/html":
            body = b"<html>not json</html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/redir-off":
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/offorigin" % self.server.server_port)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        # /pages/<id>?page=N : 3 rows a page, 5 pages, next page by NEXT_URL, Link header and cursor.
        if parts[:1] == ["pages"]:
            page = int(q.get("page", "1"))
            rows = [{"id": (page - 1) * 3 + i} for i in range(1, 4)]
            nxt = "%s/pages/%s?page=%d" % (self.base(), parts[1], page + 1) if page < 5 else None
            headers = {}
            if nxt:
                headers["Link"] = '<%s>; rel="next", <%s/pages/%s?page=5>; rel="last"' % (nxt, self.base(), parts[1])
            return self.send_json({"data": rows, "next": nxt, "cursor": str(page + 1) if page < 5 else None},
                                  headers=headers)

        # /linkrel : the Link target is a relative path.
        if path == "/linkrel":
            page = int(q.get("page", "1"))
            headers = {"Link": '</linkrel?page=%d>; rel=next' % (page + 1)} if page < 3 else {}
            return self.send_json([{"id": page}], headers=headers)

        # /linkoff : the Link target is another origin (localhost, not 127.0.0.1).
        if path == "/linkoff":
            return self.send_json([{"id": 1}], headers={
                "Link": '<http://localhost:%d/offorigin>; rel="next"' % self.server.server_port})

        # /linkloop : the Link target is this same URL, forever.
        if path == "/linkloop":
            return self.send_json([{"id": n}], headers={"Link": '<%s/linkloop>; rel="next"' % self.base()})

        # /cursorloop : the cursor never advances.
        if path == "/cursorloop":
            return self.send_json({"data": [{"id": n}], "cursor": "same"})

        self.send_json({"error": "no route", "path": path}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode()
        count(urlparse(self.path).path)
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        if parts[:1] == ["flaky"]:
            n = count("post-" + parts[1])
            if n <= 1:
                return self.send_json({"error": "busy"}, 503)
        self.send_json([{"echo": body}])


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # a client that gave up on a stalled response is expected here


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    args = ap.parse_args()
    server = QuietServer(("127.0.0.1", args.port), Handler)
    print("PORT=%d" % server.server_port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)
