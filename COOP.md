# Co-op (two players)

Two players play one Fallout game together. Player 1 hosts and runs the
whole game; player 2 has their own character (stats, skills, perks, traits,
level, inventory, caps, kill counts) and plays along on the same map.

Co-op is off unless `fallout.cfg` has a `[coop]` section with
`enabled=1`. Without it the game behaves exactly like Fallout CE (this is
verified byte for byte against upstream, see "Tests").

## Rules

- Player 2 creates their own character on the regular character creation
  screen and picks a look (vault jumpsuit, leather jacket or robe). The
  other player is drawn with an outline: player 1 green, player 2 yellow
  (each player can pick other colors under MULTIPLAYER, MORE, OUTLINES).
- Both players get full XP for everything (kills, quests, skills),
  whoever did it. Each levels up and picks perks on their own.
- Karma and reputation are shared. Drug addictions are per player.
- Perks/traits that affect everyone (Jinxed, Friendly Foe) apply if
  either player has them.
- Only player 1 talks to people, travels (world map, exit grids), rests
  and saves. Player 2 follows map and elevation changes automatically.
- Player 2 trades directly: walking up to a merchant opens the barter
  screen.
- In combat each player plays their own turn. `turn_time` limits a turn.
- If either player dies, the game is lost.
- Press T to chat (over the network). Enter sends, Esc cancels.
- If player 2 leaves, their character waits out of sight and comes back
  next to player 1 when they join again.
- When a player is in a menu (inventory, character screen, Pip-Boy,
  dialog, barter...) or it is their combat turn, the game waits for them
  and the other player sees their screen with a status line.

## Getting started

1. Start the game. The first time, if it cannot find your Fallout files,
   a setup terminal lists the installations it found (Steam, GOG, Heroic,
   ...). Pick yours, or type/paste the folder that has MASTER.DAT and
   CRITTER.DAT. Your saves stay in that installation's DATA folder.
2. In the main menu choose **MULTIPLAYER**:
   - **HOST GAME** (player 1): then NEW GAME or LOAD GAME, then one of
     LAN ONLY (same network), LIST ONLINE (in everyone's server browser)
     or PRIVATE CODE (only with the game code). Online games go through
     the co-op server's relay, so nobody opens ports; they get a code like
     KQX472. Player 2 can join any time; they create their character when
     they join (or continue the one in the save).
   - **JOIN GAME** (player 2): the server browser lists open games on
     your network (found with a UDP search on the game port) and listed
     online; pick one, or ENTER ADDRESS and type an address or a game
     code. Your screen then shows the game from player 1's computer; F10
     leaves.
   - **MORE**: ONE PC (both players on one computer; press F9 in the
     game), OUTLINES (the players' outline colors on your screen) and
     RESOLUTION (the screen size, used from the next start).
   - **UPDATE**: looks for a newer version on the co-op server and, if
     you say INSTALL, downloads it, checks it, replaces the game's files
     and starts the game again.
   - **REPORT BUG**: sends a short description and the recent log to the
     co-op server (or saves it next to the game if there is none).

Both computers need the same Fallout version, language and patches; the
host refuses a client whose game data differs. On a LAN the host's
firewall must allow port 27015 (TCP to play, UDP to be found). Player 1
sends player 2 the picture: about 0.1-0.3 MB/s, scaled to player 2's
screen size and packed, and never more than player 2's line takes.

## Without the menu

Everything can also be set in `fallout.cfg`. Host:

```ini
[coop]
enabled=1
mode=host
```

Client (starts straight into the host's game):

```ini
[coop]
enabled=1
mode=client
host=192.168.1.10
```

One PC: just `enabled=1`, then F9 in a game.

## Co-op server (updates, bug reports, server browser, relay)

`tools/coop_server.py` is a small standard-library Python server for
UPDATE, REPORT BUG, crash reports, the online server browser and the relay
that online games go through (see `docs/coop/SERVER.md`). Run it on a
machine everyone can reach:

```sh
tools/coop_server.py --port 8080 --root /srv/fallout-coop
```

Put `latest.txt` and the packages in `/srv/fallout-coop/public`.
`tools/build-release.sh <out dir> http://your.server:8080` builds both
packages and writes a matching `latest.txt`:

```
version=1.1
notes=Fixes the trading crash
url_linux=http://your.server:8080/fallout-coop/fallout-coop-1.1-linux.tar.gz
url_windows=http://your.server:8080/fallout-coop/fallout-coop-1.1-windows.zip
sha256_linux=...
sha256_windows=...
```

The game installs a package only if its SHA-256 matches. Packages hold
one folder with the game's files; they are unpacked with the system's
`tar` (built into Windows 10 and later) over the game's folder.

Reports arrive in `/srv/fallout-coop/reports`. Build releases with the
server built in, so players need no setup:

```sh
cmake -B build -DCOOP_SERVER=http://your.server:8080
```

(`server=` under `[coop]` in `fallout.cfg` overrides it.) Without a
server the game still works on the LAN and with ENTER ADDRESS; the online
options and UPDATE need one.

## Settings

| Key | Meaning |
|---|---|
| `enabled` | `1` turns co-op on. |
| `mode` | `host`, `client`, or absent for both players on one computer. |
| `host` | Client: the host's address. |
| `port` | TCP port (default 27015). |
| `turn_time` | Seconds per combat turn for human players, `0` = no limit. |
| `outline_player1`, `outline_player2` | Outline colors on this screen (0 green, 1 yellow, 2 white, 3 orange, 4 pink, 5 blue). |
| `colorblind` | Older setting: `1` means white and orange when the two above are absent. |
| `server` | Co-op server for UPDATE, REPORT BUG and the server browser, `http://host:port`. |
| `relay` | Relay for online games: `http://host/fallout-coop/relay` or `host:port`. Default: the server's host, port 80. |
| `selftest` | Developer: `1` enables the F8 self-test of player switching. |

## Saves

Saves made with a player 2 contain an extra co-op block at the end; they
are not meant to be loaded by builds without co-op. Saves without a player
2, and saves from the original game, are unchanged and load as before.

## Tests

The game has a headless test harness; `docs/coop/TESTING.md` describes it.
Tests need `FALLOUT_DATA` pointing at a Fallout installation, which is
never written to. Game data must never be committed to this repository.

How the co-op code works: `docs/coop/ARCHITECTURE.md`.
