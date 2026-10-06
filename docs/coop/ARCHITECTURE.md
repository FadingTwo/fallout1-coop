# How co-op works

This is a guide to the co-op code for people who want to change it. For
what players see, read `COOP.md` first.

The mod is built on [Fallout Community Edition](https://github.com/alexbatalov/fallout1-ce),
a re-implementation of the original engine. Co-op is off unless
`fallout.cfg` turns it on, and with it off the game must behave exactly
like Fallout CE: `tools/regress.sh compare` checks that byte for byte.
Every change to engine code (`src/game`, `src/plib`) has to keep that
true. Engine changes that are bug fixes are marked `// CE:`.

## The big picture

One computer runs the game. That is player 1, the host. Player 2's
computer runs no game logic at all: it sends its keyboard and mouse to the
host and shows the pictures the host sends back. This "thin client" design
keeps the two games from drifting apart, because there is only one game.

```
 player 2's computer                       player 1's computer (host)
 ┌─────────────────────┐   input (TCP)    ┌──────────────────────────────┐
 │ coop_net_client_run │ ───────────────▶ │ the whole game               │
 │   shows frames,     │                  │  player 1: local input       │
 │   plays sounds      │ ◀─────────────── │  player 2: client's input,   │
 └─────────────────────┘  frames, sounds  │   own camera, own view       │
                                          └──────────────────────────────┘
```

Online the bytes go through a relay on the co-op server instead of
directly (see "Network").

## Two players in a one-player engine

The engine has one player: `obj_dude` and many globals (stats, perks,
traits, skills, kill counts, the active hand...). Co-op keeps one
`PlayerState` (`coop.h`) per player holding all of that, and makes a
player "active" by swapping their state into the engine's globals:

```cpp
{
    ActivePlayerScope scope(coop_player(1)); // obj_dude is now player 2
    critter_adjust_rads(obj_dude, 50);       // engine code works unchanged
}                                            // player 1 again
```

`ActivePlayerScope` is the only way the active player changes. Scopes
nest, and the destructor always switches back. The script interpreter
uses `longjmp`, which skips destructors, so `coop_scope_unwind_to` exists
for that one case.

Most features follow from asking "whose turn/menu/action is this?" and
running that code in that player's scope. Examples:

- Experience: `stat_pc_add_experience` (`stat.cc`) gives every player the XP the engine gives
  `obj_dude`, each in their own scope.
- Radiation sickness: `scripts.cc` checks every player in their scope.
- Quest items: `intextra.cc` looks in both players' packs.
- Combat: each player's turn runs in their scope (`combat.cc`).

What stays shared on purpose: karma, reputation, the map, time and the
party.

## The host's frame

`coop_host.cc` runs inside the normal main loop:

1. Player 1's input is handled as usual.
2. `coop_host_frame()`: the client's input is fed in as player 2's input
   (`coop_host_feed_input`, using the input injector in
   `plib/gnw/inject.cc`). Player 2's camera, mouse and cursor mode are
   swapped in, player 2's view is rendered, sent to the client, and player
   1's view is swapped back.
3. `renderPresent()` shows player 1's view.

Menus and fights use a different mode: the screen is shared. Whoever owns
the menu (or the combat turn) drives the input, and the other player sees
the same screen with a status line. The engine's menus are modal loops,
so the host hooks `renderPresent` (`svga_present_hook`) to keep sending
frames while one runs.

## Network

`coop_net.cc`. TCP, port 27015 by default. Every message is
`u32 length, u8 type, payload` (length counts the type byte):

| Type | Direction | Payload |
|---|---|---|
| HELLO (1) | client to host | magic, protocol version, game data checksum, flags (1 colorblind, 2 acknowledges frames, 4 takes packed frames), outline colors, screen size |
| WELCOME (2) | host to client | accepted, reason, host screen size |
| INPUT (3) | client to host | a key or the mouse position and buttons |
| FRAME (4) | host to client | width, height, palette if changed, then runs of unchanged/changed pixels |
| STATUS (5) | host to client | the status line ("Player 1's turn.", chat typing...) |
| SOUND (6) | host to client | sound effect, music or speech to play |
| BYE (7) | both | leaving |
| ACK (8) | client to host | frames received so far |
| FRAME_PACKED (9) | host to client | a FRAME, LZ77-packed |

Fields added later are appended to existing messages, so older and newer
versions still talk to each other: readers check `atEnd()` before reading
optional fields, and unknown message types are ignored.

Frames are 8-bit palette pictures. The host keeps the last frame it sent
and sends only the pixels that changed. Three things keep online play
smooth:

- Flow control: the client acknowledges frames, and the host keeps at most
  two frames on the way. On a slow line the next frame simply carries all
  changes since, instead of a queue of old frames building up.
- Frames bigger than the client's screen are scaled down before sending
  (the client would scale them down anyway). Mouse positions are scaled
  back up on the host.
- FRAME_PACKED: the frame compressed with a small LZ77 (`coop_net_compress`).

The client (`coop_net_client_run`) scales the frames to its window,
keeping their shape, and maps its mouse back into frame coordinates.

### Finding games and the relay

- LAN: hosts answer a UDP query on the game port (`coop_lobby.cc`).
- Online: hosts announce themselves to the co-op server over HTTP, and
  clients fetch the list.
- Relay: online, both games connect *out* to the co-op server, which
  pipes the bytes between them, so nobody needs an open port. The host
  opens a "room" and gets a code; the client joins with the code. The
  relay sits behind a web server on port 80 (an HTTP upgrade first), which
  works through almost any firewall. Protocol details are at the top of
  `tools/coop_server.py`.

## Files

| File | What it does |
|---|---|
| `coop.h/.cc` | Player states, scopes, saving the co-op block, rules (XP, karma, outlines...). |
| `coop_host.h/.cc` | The host's per-frame work, player 2's view, chat, away/back. |
| `coop_net.h/.cc` | Sockets, messages, frames, relay client, the client's main loop. |
| `coop_lobby.h/.cc` | Server browser: LAN search and the online list. |
| `coop_menu.h/.cc` | The MULTIPLAYER menu (host, join, more, update, report bug). |
| `coop_setup.h/.cc` | The first-run setup terminal that finds Fallout's data. |
| `coop_update.h/.cc` | Self-update: download, verify SHA-256, replace files. |
| `coop_http.h/.cc` | A tiny HTTP client: WinHTTP on Windows, sockets plus the system's OpenSSL elsewhere. |
| `coop_crash.h/.cc` | Crash reports written by signal / exception handlers. |
| `coop_autotest.cc` | The co-op test scripts (see `TESTING.md`). |
| `tools/coop_server.py` | The co-op server: updates, reports, lobby, relay, play statistics. |

## Saves

A save with a player 2 has a co-op block after the normal data
(`coop_save`/`coop_load`). Saves without player 2 are byte for byte the
engine's own, so they load in Fallout CE.

## Rules of thumb for changes

- Keep single-player identical. Run `tools/regress.sh compare` before
  committing engine changes.
- Ask whose scope code runs in. Bugs in this mod are usually "this ran as
  player 1 but was about player 2", or the other way round.
- Network changes must keep older versions working: append fields, don't
  change existing ones.
- Add a test script for each bug you fix, if it can be tested.
