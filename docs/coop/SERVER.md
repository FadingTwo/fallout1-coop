# Running your own co-op server

The game works without any server on a LAN, and with ENTER ADDRESS when
player 1's port is reachable. A server adds:

- UPDATE: players download new versions from it.
- REPORT BUG and crash reports: reports are saved on it.
- The online server browser (LIST ONLINE).
- The relay: online games (LIST ONLINE, PRIVATE CODE) go through it, so
  players don't open ports.

`tools/coop_server.py` does all of this with only Python's standard
library.

## Start it

```sh
tools/coop_server.py --port 8080 --root /srv/fallout-coop --relay-port 27016
```

- `--root` holds `public/` (served at `/fallout-coop/`: `latest.txt` and
  the packages), `reports/` (bug and crash reports) and small data files
  (`play-stats.json`).
- `--relay-port` turns on the relay. Players' games reach it either
  directly (`[coop] relay=host:27016`) or through a web server (below).

## Point the game at it

Build your releases with the server built in:

```sh
cmake -B build -DCOOP_SERVER=https://your.server
tools/build-release.sh dist https://your.server
```

Or set it per player in `fallout.cfg`:

```ini
[coop]
server=https://your.server
relay=http://your.server/fallout-coop/relay
```

Without `relay`, the game uses `http://<server's host>/fallout-coop/relay`.

The source has no server built in. Nothing in it talks to the internet
unless a server is set.

## Behind a web server

Most home and office networks allow web traffic only, so the relay works
best behind a web server on port 80. An nginx example (the relay is
reached with any host name you choose; the game sends
`Host: fallout-coop-relay`):

```nginx
server {
    listen 80;
    server_name fallout-coop-relay;
    location = /fallout-coop/relay {
        proxy_pass http://127.0.0.1:27016;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header X-Real-IP $remote_addr;
        proxy_buffering off;
        proxy_read_timeout 1h;
        proxy_send_timeout 1h;
    }
}
```

And for the rest, in your site's HTTPS server:

```nginx
location /fallout-coop/ { proxy_pass http://127.0.0.1:8080/fallout-coop/; proxy_set_header X-Real-IP $remote_addr; }
```

## Publishing a version

1. `tools/build-release.sh dist https://your.server` builds the Linux and
   Windows packages, `latest.txt` (version, notes, URLs, SHA-256) and keeps
   symbols in `dist/symbols/<version>`.
2. Edit `notes=` in `dist/latest.txt`.
3. Copy the packages and `latest.txt` to `<root>/public/`.

Players' games offer the update the next time they open MULTIPLAYER.
They install it only if the SHA-256 matches.

`tools/deploy-server.sh` copies a new `coop_server.py` to a server over
ssh and restarts it, but only when no relayed game is running
(`DEPLOY_TARGET=user@host`).

## Limits

The relay passes every relayed game's traffic: about 0.1-0.3 MB/s each
way per game. `RELAY_MAX_PIPES` (25 games) and the other limits at the top
of `coop_server.py` protect a home connection. Joins are limited per
address so game codes can't be guessed.

`GET /fallout-coop/status` (only from the server itself) returns the games
in progress and daily counts, for an admin page.
