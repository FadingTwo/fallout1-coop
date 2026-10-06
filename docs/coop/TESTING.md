# Testing

The game has a headless test harness: a build plays itself with scripted
input, takes screenshots, checks things and writes a result. Two players
are two processes talking over localhost, so the real network code runs.

You need your own copy of Fallout. Point `FALLOUT_DATA` at the folder with
`MASTER.DAT`, `CRITTER.DAT` and `DATA`. The tests never write there: the
`.DAT` files are linked and `DATA` is copied into the test's own folder.

```sh
export FALLOUT_DATA=~/Games/Fallout
cmake -B build && cmake --build build
```

## Running tests

| Command | What it runs |
|---|---|
| `tools/regress.sh run build/fallout-ce out/a` | The single-player regression script: level-ups, perks, skills, kills, inventory, saving and loading. |
| `tools/regress.sh compare out/a out/b` | Checks two runs played byte for byte the same (state dumps and saves). |
| `AUTOTEST_SCRIPT=coop tools/regress.sh run build/fallout-ce out/c "[coop]enabled=1"` | Any one-process script by name. |
| `tools/nettest.sh build/fallout-ce out/n net_play_host net_play_client` | A host and a client. |
| `tools/soak.sh out/soak` | Random scenarios until stopped (see below). |

The most important check: single player must not change. Build upstream
Fallout CE (or an older version of this mod), run `regress.sh run` with
both, and `compare` them. Every engine change has to pass this.

Results are in `<out>/autotest/result.txt` (and `debug.log`,
`stdout.log`, screenshots `scr*.bmp`). `nettest.sh` makes `<out>/host` and
`<out>/client`.

## Scripts

All scripts live in `src/game/autotest.cc` (single player) and
`src/game/coop_autotest.cc` (co-op).

One process: `regress`, `coop` (players and switching), `coop_travel`,
`coop_combat`, `coop_join` (character creation), `coop_looks`,
`coop_barter`, `coop_rads`, `coop_interact` (pick up, doors, stimpak),
`story_kill`, `pack` (frame packing), `map_time`, `movie`, `credits`, and
the menu scripts `main_menu`, `menu_flow`, `menu_join`, `menu_server`,
`menu_more`, `menu_host_code`, `menu_crash_report`.

Two processes (`nettest.sh <bin> <out> <host script> <client script>`):

| Host | Client | What it covers |
|---|---|---|
| `net_host` | `net_client` or `menu_join` | Joining, the server browser. |
| `net_play_host` | `net_play_client` | Character creation over the network, walking, inventory, chat. |
| `net_combat_host` | `net_combat_client` | Turn-based fights with both players. |
| `net_session_host` | `net_session_client` | Leave and rejoin, saving and loading with player 2 there, trading, the turn timer. |
| `net_death_host` | `net_client` | Either player dying ends the game properly. |
| `net_ending_host` | `net_client` | The ending: movie and credits with player 2 watching. |
| `net_tour_host` | `net_tour_client` | Both players visit every map (`COOP_AUTOTEST_TOUR=ALL`). |

## Settings for tests

`nettest.sh`: `HOST_RES` / `CLIENT_RES` (screen sizes, e.g. `1920x1080`),
`HOST_EXTRA` / `CLIENT_EXTRA` (extra `fallout.cfg` settings),
`CLIENT_BIN` (another binary for player 2, e.g. a Windows build under
Wine), `HOST_WAIT` (seconds before the client starts).

`regress.sh`: `AUTOTEST_SCRIPT`, `AUTOTEST_TIMEOUT` (seconds),
`AUTOTEST_RES`, `AUTOTEST_SAVES` (start from saved games),
`AUTOTEST_CRASH_REPORT` (put a crash report in place first).

Environment for the scripts (read with `getenv` in `coop_autotest.cc`):

| Variable | Effect |
|---|---|
| `COOP_AUTOTEST_TOUR` | `ALL` or a list `A.MAP,B.MAP`. |
| `COOP_AUTOTEST_ROUGH` | Fights are played quickly and players are kept alive. |
| `COOP_AUTOTEST_SEED` | Shuffles the tour and where player 2 walks; the same seed repeats a run. |
| `COOP_AUTOTEST_RELAY` / `_RELAY_FILE` / `_RELAY_CODE` | Connect through a relay (e.g. a local `coop_server.py --relay-port`). |
| `COOP_AUTOTEST_STORY` | `CHIP,VATS,MASTER`: start with those story steps done. |
| `COOP_AUTOTEST_KILL`, `_LIST`, `_STAY_MS` | Kill a named critter, list critters, stay longer on each map. |
| `COOP_AUTOTEST_COMBAT_CHAT` | Both players chat during a fight. |
| `COOP_AUTOTEST_VIDEO` | Screenshots become video frames (`tools/make_video.py`). |
| `COOP_NET_NO_FLOW`, `_NO_ACKS`, `_NO_PACKING` | Turn off the newer network features, to compare or test old clients. |
| `COOP_HEAP_CHECK` | Checks the engine heap's guards after every heap call and stops at the first damage. |
| `FALLOUT_COOP_CRASH_TEST=crash` | Crashes on purpose (tests crash reports). |

`coop_server.py` for tests: `COOP_RELAY_TEST_RATE` (limits the relay to
that many bytes a second, like a slow line) and `COOP_RELAY_IDLE_TIMEOUT`.

## Soak testing

`tools/soak.sh` runs scenarios at random until `SOAK_UNTIL=HH:MM` or
`SOAK_RUNS=n`: the tour, the session or a fight; directly, through a
relay, through a slow relay, under the address sanitizer
(`SOAK_ASAN_BIN`) or with a Windows player 2 (`SOAK_WINE`); random screen
sizes and a random seed. Every run adds a line to `summary.log` with what
you need to repeat it; failed runs keep their logs in `fail-<n>`.

## Other tools

- Address sanitizer: configure a second build with
  `-DCMAKE_CXX_FLAGS=-fsanitize=address` and use it as the binary.
- Windows: `tools/build-release.sh` cross-compiles with zig. Run the exe
  under Wine as `CLIENT_BIN` or as the host.
- Crash reports: `tools/crash-symbols.sh <report>` turns the addresses into
  functions and lines, using the symbols `build-release.sh` keeps.
- The Windows build is compiled by clang, which uses undefined behaviour
  more aggressively than GCC. A `call` followed by `int3` in the
  disassembly marks code clang decided can't be reached; new ones after a
  change deserve a look.
