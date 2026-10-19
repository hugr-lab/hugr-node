#!/usr/bin/env python3
"""A stand-in for B's front (spec 011's e2e).

It routes `POST /quack` by the node label a connection id starts with (`X-Quack-Connection-Id: <label>.<random>`,
duckdb-acl spec 134) and places a new connection (a request without the header) on the node it is told. What a real
front does with B's discovery and load is told here by the script:
  GET /control/place?node=<label>    new connections go to that node
  GET /control/forget?node=<label>   the front lost that node: its label's requests go to the placed node, which does
                                     not know them ("not known on this node - connect again")
  GET /control/know?node=<label>     the node is back
  GET /control/slow?node=<label>&ms=<n>  every request to that node waits n ms first (a long scan, to kill B under)
Usage: front.py <port> <label>=<host:port> [<label>=<host:port> ...]   (the first node is placed first)
"""
import http.server
import socketserver
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BACKENDS = {}
STATE = {"place": None, "forgotten": set(), "slow": {}}
LOCK = threading.Lock()
HOP = {"host", "content-length", "connection", "transfer-encoding", "keep-alive"}


class Front(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        node = urllib.parse.parse_qs(url.query).get("node", [""])[0]
        with LOCK:
            if url.path == "/control/place" and node in BACKENDS:
                STATE["place"] = node
            elif url.path == "/control/forget" and node in BACKENDS:
                STATE["forgotten"].add(node)
            elif url.path == "/control/know" and node in BACKENDS:
                STATE["forgotten"].discard(node)
            elif url.path == "/control/slow" and node in BACKENDS:
                STATE["slow"][node] = int(urllib.parse.parse_qs(url.query).get("ms", ["0"])[0])
            else:
                return self.reply(404, b"no such control")
        self.reply(200, b"ok")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        connection_id = self.headers.get("X-Quack-Connection-Id", "")
        label = connection_id.split(".", 1)[0] if "." in connection_id else ""
        with LOCK:
            node = label if label in BACKENDS and label not in STATE["forgotten"] else STATE["place"]
            delay = STATE["slow"].get(node, 0)
        if delay:
            time.sleep(delay / 1000)
        request = urllib.request.Request("http://" + BACKENDS[node] + self.path, data=body, method="POST")
        for name, value in self.headers.items():
            if name.lower() not in HOP:
                request.add_header(name, value)
        try:
            response = urllib.request.urlopen(request, timeout=600)
        except urllib.error.HTTPError as error:
            response = error
        except (urllib.error.URLError, OSError) as error:
            return self.reply(502, ("front: " + node + " did not answer: " + str(error)).encode())
        data = response.read()
        headers = [(n, v) for n, v in response.headers.items() if n.lower() not in HOP]
        self.reply(response.status, data, headers)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    port = int(sys.argv[1])
    for spec in sys.argv[2:]:
        label, address = spec.split("=", 1)
        BACKENDS[label] = address
        STATE["place"] = STATE["place"] or label
    Server(("127.0.0.1", port), Front).serve_forever()


if __name__ == "__main__":
    main()
