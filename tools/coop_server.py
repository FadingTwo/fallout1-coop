#!/usr/bin/env python3
"""Reference server for the co-op menu's UPDATE and REPORT BUG buttons.

    tools/coop_server.py --port 8080 --root /srv/fallout-coop

Players set it in fallout.cfg:

    [coop]
    server=http://your.server:8080

Endpoints (plain HTTP):

    GET  /fallout-coop/<file>   files from <root>/public, e.g. latest.txt:
                                    version=1.1
                                    notes=Fixes trading crash
                                    url_windows=http://your.server:8080/fallout-coop/fallout-coop-1.1-windows.zip
                                    url_linux=http://your.server:8080/fallout-coop/fallout-coop-1.1-linux.zip
                                    url_macos=...   (or url=... for all)
    POST /fallout-coop/report   saves the report (text, at most 1 MB) as
                                <root>/reports/<time>-<address>.txt
    GET/POST /fallout-coop/forum/...   an optional website forum (coop_forum.py, if present)
    POST /fallout-coop/lobby    a host lists its game (action=announce, port,
                                players, version, checksum, name; one per
                                line) or removes it (action=remove, port)
    GET  /fallout-coop/lobby    the listed games, one per line:
                                address|port|players|version|checksum|name|relay
                                (a game is dropped 90 s after its last
                                announce; games with a relay code are
                                joined through the relay and their
                                address is not shown)

Relay (with --relay-port, plain TCP): players connect through this server,
so player 1 needs no open port. Behind nginx on port 80 a connection may
start with an HTTP upgrade (GET /fallout-coop/relay, Upgrade: fcoop-relay;
answered with 101, X-Real-IP gives the player's address). Then each
connection starts with one line:

    FCOOP-RELAY HOST <code or ->      player 1's control connection; answer
                                      "ROOM <code>", then "CONN <id>" for
                                      each player 2 (and PING now and then)
    FCOOP-RELAY JOIN <code>           player 2; answer "OK" once player 1
                                      accepted (or "ERR <reason>"), then the
                                      game's own bytes
    FCOOP-RELAY ACCEPT <code> <id>    player 1's connection for CONN <id>;
                                      no answer, then the game's own bytes

Only the standard library is needed.
"""

import argparse
import datetime
import os
import posixpath
import random
import socket
import threading
import time
import json
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# An optional forum for a website, if coop_forum.py is next to this file.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from coop_forum import Forum  # noqa: E402
except ImportError:
    Forum = None

MAX_REPORT = 1024 * 1024
PREFIX = "/fallout-coop/"

# Spam limits: reports per address per hour, and in total on disk.
REPORTS_PER_HOUR = 10
MAX_REPORT_FILES = 5000
MAX_REPORTS_SIZE = 200 * 1024 * 1024

_recent = {}
_recent_lock = threading.Lock()

# Server browser: (address, port) -> fields, last seen.
LOBBY_TIMEOUT = 90
LOBBY_MAX_GAMES = 500
LOBBY_MAX_PER_ADDRESS = 4
_lobby = {}
_lobby_lock = threading.Lock()


# Relay: code -> room; limits against abuse.
RELAY_MAX_ROOMS = 300
RELAY_MAX_ROOMS_PER_ADDRESS = 8
# Each game sends about 0.1-0.3 MB/s each way through the server.
RELAY_MAX_PIPES = 25
RELAY_MAX_PENDING = 4
RELAY_ACCEPT_TIMEOUT = 15
RELAY_IDLE_TIMEOUT = int(os.environ.get("COOP_RELAY_IDLE_TIMEOUT", 20 * 60))
# Testing only: limit each direction of a relayed game to this many bytes a
# second, like a slow internet line.
RELAY_TEST_RATE = int(os.environ.get("COOP_RELAY_TEST_RATE", 0))
# Joins per address per 10 minutes, so codes can't be guessed by trying
# them all (private games are only as private as their code).
RELAY_JOINS_PER_WINDOW = 30
RELAY_JOIN_WINDOW = 600
RELAY_CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ"
RELAY_CODE_DIGITS = "23456789"
_rooms = {}
_relay_lock = threading.Lock()
_relay_pipes = 0
_relay_next_id = 1
_relay_joins = {}


# Play statistics for the admin panel (GET /fallout-coop/status, only from
# this machine): what's going on now, and per day online hosts, relayed
# games and minutes played, kept in <root>/play-stats.json.
_stats_lock = threading.Lock()
_stats = {"days": {}}
_stats_path = None
_relay_games = {}  # pipe id -> {"code", "since"}


def _stats_load(root):
    global _stats_path, _stats
    _stats_path = os.path.join(root, "play-stats.json")
    try:
        with open(_stats_path) as f:
            _stats = json.load(f)
    except (OSError, ValueError):
        _stats = {"days": {}}


def _stats_add(key, amount=1):
    day = time.strftime("%Y-%m-%d")
    with _stats_lock:
        entry = _stats["days"].setdefault(day, {})
        entry[key] = entry.get(key, 0) + amount
        if _stats_path is not None:
            try:
                with open(_stats_path + ".tmp", "w") as f:
                    json.dump(_stats, f)
                os.replace(_stats_path + ".tmp", _stats_path)
            except OSError:
                pass


def _status():
    now = time.time()
    with _relay_lock:
        rooms = len(_rooms)
        games = [{"code": g["code"], "minutes": int((now - g["since"]) / 60)} for g in _relay_games.values()]
    with _lobby_lock:
        _lobby_expire(now)
        listed = [{"name": g["name"], "version": g["version"], "players": g["players"], "online": bool(g["relay"])}
                  for g in _lobby.values()]
    with _stats_lock:
        days = dict(sorted(_stats["days"].items())[-30:])
    return {"now": {"online_hosts": rooms, "relayed_games": games, "listed": listed}, "days": days}


def _relay_new_code(wanted):
    wanted = wanted.upper()
    if (len(wanted) == 6 and all(c in RELAY_CODE_LETTERS for c in wanted[:3])
            and all(c in RELAY_CODE_DIGITS for c in wanted[3:]) and wanted not in _rooms):
        return wanted
    for _ in range(100):
        code = "".join(random.choice(RELAY_CODE_LETTERS) for _ in range(3)) + \
            "".join(random.choice(RELAY_CODE_DIGITS) for _ in range(3))
        if code not in _rooms:
            return code
    return None


def _relay_read_line(conn, limit=128):
    data = b""
    while not data.endswith(b"\n"):
        chunk = conn.recv(1)
        if not chunk:
            return None
        data += chunk
        if len(data) > limit:
            return None
    return data.decode("ascii", "replace").strip()


def _relay_pipe(a, b):
    """Copies both ways until either side closes, or both go quiet (one
    side may stay silent for long, e.g. player 2 watching)."""
    global _relay_pipes
    last = [time.time()]

    def copy(src, dst):
        sent = 0
        since = time.time()
        try:
            while True:
                try:
                    data = src.recv(65536 if not RELAY_TEST_RATE else 4096)
                except socket.timeout:
                    if time.time() - last[0] > RELAY_IDLE_TIMEOUT:
                        break
                    continue
                if not data:
                    break
                last[0] = time.time()
                if RELAY_TEST_RATE:
                    # A slow line, for testing: at most this many bytes a second.
                    sent += len(data)
                    ahead = sent / RELAY_TEST_RATE - (time.time() - since)
                    if ahead > 0:
                        time.sleep(ahead)
                dst.sendall(data)
        except OSError:
            pass
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    for s in (a, b):
        s.settimeout(min(60, RELAY_IDLE_TIMEOUT))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    other = threading.Thread(target=copy, args=(b, a), daemon=True)
    other.start()
    copy(a, b)
    other.join()
    a.close()
    b.close()
    with _relay_lock:
        _relay_pipes -= 1


def _relay_host(conn, address, wanted):
    with _relay_lock:
        if len(_rooms) >= RELAY_MAX_ROOMS or sum(1 for r in _rooms.values() if r["address"] == address) >= RELAY_MAX_ROOMS_PER_ADDRESS:
            conn.sendall(b"ERR The server is busy. Try again later.\n")
            conn.close()
            return
        # The same host coming back (its old connection may not look dead
        # yet): it gets its code back, the old connection goes.
        old = _rooms.get(wanted.upper())
        if old is not None and old["address"] == address:
            del _rooms[wanted.upper()]
            try:
                old["control"].shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        code = _relay_new_code(wanted)
        if code is None:
            conn.close()
            return
        room = {"control": conn, "address": address, "pending": {}, "send_lock": threading.Lock()}
        _rooms[code] = room
    _stats_add("online_hosts")
    print("relay: room %s for %s" % (code, address), flush=True)
    try:
        conn.sendall(("ROOM %s\n" % code).encode())
        conn.settimeout(25)
        while True:
            try:
                if not conn.recv(64):
                    break
            except socket.timeout:
                with room["send_lock"]:
                    conn.sendall(b"PING\n")
    except OSError:
        pass
    with _relay_lock:
        if _rooms.get(code) is room:
            del _rooms[code]
    conn.close()
    print("relay: room %s closed" % code, flush=True)


def _relay_join(conn, code, address):
    global _relay_next_id, _relay_pipes
    code = code.upper().replace("-", "")
    with _relay_lock:
        now = time.time()
        times = [t for t in _relay_joins.get(address, []) if now - t < RELAY_JOIN_WINDOW]
        if len(times) >= RELAY_JOINS_PER_WINDOW:
            _relay_joins[address] = times
            conn.sendall(b"ERR Too many tries. Wait a few minutes.\n")
            conn.close()
            return
        times.append(now)
        _relay_joins[address] = times
        if len(_relay_joins) > 10000:
            for key in [k for k, v in _relay_joins.items() if not v or now - v[-1] >= RELAY_JOIN_WINDOW]:
                del _relay_joins[key]
        room = _rooms.get(code)
        if room is None:
            print("relay: join %s from %s: no such game" % (code, address), flush=True)
            conn.sendall(b"ERR No game with that code.\n")
            conn.close()
            return
        if _relay_pipes >= RELAY_MAX_PIPES:
            conn.sendall(b"ERR Our server is full. Try again later.\n")
            conn.close()
            return
        if len(room["pending"]) >= RELAY_MAX_PENDING:
            conn.sendall(b"ERR The server is busy. Try again later.\n")
            conn.close()
            return
        pending_id = _relay_next_id
        _relay_next_id += 1
        waiting = {"event": threading.Event(), "partner": None}
        room["pending"][pending_id] = waiting
    try:
        with room["send_lock"]:
            room["control"].sendall(("CONN %d\n" % pending_id).encode())
        waiting["event"].wait(RELAY_ACCEPT_TIMEOUT)
    except OSError:
        pass
    with _relay_lock:
        # Once removed, a late ACCEPT finds nothing; one that came in time
        # is used even if the wait just timed out.
        room["pending"].pop(pending_id, None)
        partner = waiting["partner"]
        if partner is not None:
            _relay_pipes += 1
    if partner is None:
        print("relay: join %s from %s: player 1 did not answer" % (code, address), flush=True)
        try:
            conn.sendall(b"ERR Player 1 did not answer.\n")
        except OSError:
            pass
        conn.close()
        return
    try:
        conn.sendall(b"OK\n")
    except OSError:
        partner.close()
        conn.close()
        with _relay_lock:
            _relay_pipes -= 1
        return
    started = time.time()
    print("relay: join %s from %s: playing" % (code, address), flush=True)
    with _relay_lock:
        _relay_games[pending_id] = {"code": code, "since": started}
    _stats_add("relayed_games")
    try:
        _relay_pipe(conn, partner)
    finally:
        with _relay_lock:
            _relay_games.pop(pending_id, None)
        print("relay: game %s ended after %.1f min" % (code, (time.time() - started) / 60), flush=True)
        _stats_add("minutes_played", round((time.time() - started) / 60, 1))


def _relay_accept(conn, code, pending_id):
    with _relay_lock:
        room = _rooms.get(code.upper())
        waiting = room["pending"].get(pending_id) if room is not None else None
        if waiting is None or waiting["partner"] is not None:
            conn.close()
            return
        waiting["partner"] = conn
    # The joining side's thread runs the pipe.
    waiting["event"].set()


def _relay_connection(conn, address):
    try:
        conn.settimeout(10)
        line = _relay_read_line(conn, 512)
        if line is not None and line.startswith("GET "):
            # Through nginx: the HTTP upgrade first.
            upgrade = False
            for _ in range(40):
                header = _relay_read_line(conn, 512)
                if header is None:
                    conn.close()
                    return
                if header == "":
                    break
                name, _, value = header.partition(":")
                if name.strip().lower() == "x-real-ip" and address in ("127.0.0.1", "::1"):
                    address = value.strip()
                if name.strip().lower() == "upgrade" and value.strip().lower() == "fcoop-relay":
                    upgrade = True
            if not upgrade:
                conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                conn.close()
                return
            conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: fcoop-relay\r\nConnection: Upgrade\r\n\r\n")
            line = _relay_read_line(conn)
        parts = (line or "").split()
        if len(parts) >= 3 and parts[0] == "FCOOP-RELAY" and parts[1] == "HOST":
            _relay_host(conn, address, parts[2])
        elif len(parts) >= 3 and parts[0] == "FCOOP-RELAY" and parts[1] == "JOIN":
            _relay_join(conn, parts[2], address)
        elif len(parts) >= 4 and parts[0] == "FCOOP-RELAY" and parts[1] == "ACCEPT" and parts[3].isdigit():
            _relay_accept(conn, parts[2], int(parts[3]))
        else:
            conn.close()
    except OSError:
        conn.close()


def _relay_serve(bind, port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((bind, port))
    server.listen(64)
    print("Relay on %s:%d" % (bind, port), flush=True)
    while True:
        conn, (address, _) = server.accept()
        threading.Thread(target=_relay_connection, args=(conn, address), daemon=True).start()


def _clean(text, limit):
    return "".join(ch for ch in text if 32 <= ord(ch) < 127 and ch not in "|=&")[:limit]


def _lobby_expire(now):
    for key in [k for k, v in _lobby.items() if now - v["seen"] > LOBBY_TIMEOUT]:
        del _lobby[key]


def _allow(address):
    now = time.time()
    with _recent_lock:
        times = [t for t in _recent.get(address, []) if now - t < 3600]
        if len(times) >= REPORTS_PER_HOUR:
            _recent[address] = times
            return False
        times.append(now)
        _recent[address] = times
        return True


def _reports_full(reports):
    count = 0
    size = 0
    for entry in os.scandir(reports):
        if entry.is_file():
            count += 1
            size += entry.stat().st_size
    return count >= MAX_REPORT_FILES or size >= MAX_REPORTS_SIZE


class Handler(BaseHTTPRequestHandler):
    root = "."
    forum = None

    def _send(self, status, body=b"", content_type="text/plain; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _address(self):
        # Behind a proxy (see COOP.md) the real address is in X-Real-IP.
        address = self.client_address[0]
        if address in ("127.0.0.1", "::1") and self.headers.get("X-Real-IP"):
            address = self.headers.get("X-Real-IP")
        return address

    def _forum(self, method):
        """forum/... requests; False when the path is something else."""
        url = urllib.parse.urlparse(self.path)
        if not url.path.startswith(PREFIX + "forum/") or Handler.forum is None:
            return False
        body = b""
        if method == "POST":
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if length <= 0 or length > 16 * 1024:
                self._send(400, b'{"error": "Bad request."}', "application/json")
                return True
            body = self.rfile.read(length)
        result = Handler.forum.handle(method, url.path[len(PREFIX + "forum/"):], urllib.parse.parse_qs(url.query),
                                      body, self._address())
        if result is None:
            self._send(404, b'{"error": "Not found."}', "application/json")
        else:
            self._send(result[0], json.dumps(result[1]).encode(), "application/json")
        return True

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if not path.startswith(PREFIX):
            return self._send(404, b"not found\n")
        if self._forum("GET"):
            return

        # The admin panel, on this machine only (not through the web server).
        if path == PREFIX + "status":
            if self.client_address[0] != "127.0.0.1" or self.headers.get("X-Real-IP"):
                return self._send(404, b"not found\n")
            return self._send(200, json.dumps(_status()).encode(), "application/json")

        if path == PREFIX + "lobby":
            now = time.time()
            with _lobby_lock:
                _lobby_expire(now)
                lines = ["%s|%d|%d|%s|%s|%s|%s" % ("relay" if g["relay"] else address, port, g["players"], g["version"],
                                                   g["checksum"], g["name"], g["relay"])
                         for (address, port), g in sorted(_lobby.items(), key=lambda item: -item[1]["seen"])]
            return self._send(200, ("\n".join(lines) + "\n" if lines else "").encode())

        name = posixpath.normpath(path[len(PREFIX):])
        if name.startswith("..") or name.startswith("/") or name in ("", "."):
            return self._send(404, b"not found\n")

        file_path = os.path.join(self.root, "public", *name.split("/"))
        if not os.path.isfile(file_path):
            return self._send(404, b"not found\n")

        with open(file_path, "rb") as f:
            data = f.read()

        content_type = "text/plain; charset=utf-8" if file_path.endswith(".txt") else "application/octet-stream"
        self._send(200, data, content_type)

    def _lobby_post(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > 1024:
            return self._send(400, b"bad request\n")

        fields = {}
        for line in self.rfile.read(length).decode("utf-8", "replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                fields[key.strip()] = value.strip()

        try:
            port = int(fields.get("port", ""))
            players = int(fields.get("players", "1"))
        except ValueError:
            return self._send(400, b"bad request\n")
        if not 0 < port < 65536:
            return self._send(400, b"bad request\n")

        address = self._address()
        key = (address, port)
        now = time.time()
        with _lobby_lock:
            _lobby_expire(now)
            if fields.get("action") == "remove":
                _lobby.pop(key, None)
                return self._send(200, b"removed\n")
            if fields.get("action") != "announce":
                return self._send(400, b"bad request\n")
            if key not in _lobby:
                if len(_lobby) >= LOBBY_MAX_GAMES or sum(1 for a, _ in _lobby if a == address) >= LOBBY_MAX_PER_ADDRESS:
                    return self._send(503, b"lobby is full\n")
            _lobby[key] = {
                "players": max(0, min(players, 9)),
                "version": _clean(fields.get("version", ""), 16),
                "checksum": _clean(fields.get("checksum", ""), 32),
                "name": _clean(fields.get("name", ""), 24),
                "relay": _clean(fields.get("relay", ""), 8),
                "seen": now,
            }
        return self._send(200, b"listed\n")

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if self._forum("POST"):
            return
        if path == PREFIX + "lobby":
            return self._lobby_post()
        if path != PREFIX + "report":
            return self._send(404, b"not found\n")

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1

        if length <= 0 or length > MAX_REPORT:
            return self._send(400, b"bad report\n")

        address = self._address()

        if not _allow(address):
            return self._send(429, b"too many reports, try again later\n")

        data = self.rfile.read(length)
        reports = os.path.join(self.root, "reports")
        os.makedirs(reports, exist_ok=True)

        if _reports_full(reports):
            return self._send(503, b"report box is full\n")

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        address = address.replace(":", "_").replace("/", "_")
        with open(os.path.join(reports, "%s-%s.txt" % (stamp, address)), "wb") as f:
            f.write(data)

        self._send(200, b"thanks\n")

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.client_address[0], fmt % args), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--root", default=".")
    parser.add_argument("--relay-port", type=int, default=0, help="relay players on this TCP port (0: no relay)")
    parser.add_argument("--relay-bind", default="0.0.0.0")
    args = parser.parse_args()

    if args.relay_port:
        threading.Thread(target=_relay_serve, args=(args.relay_bind, args.relay_port), daemon=True).start()

    Handler.root = os.path.abspath(args.root)
    Handler.forum = Forum(Handler.root) if Forum is not None else None
    _stats_load(Handler.root)
    os.makedirs(os.path.join(Handler.root, "public"), exist_ok=True)
    print("Serving %s on %s:%d" % (Handler.root, args.bind, args.port), flush=True)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
