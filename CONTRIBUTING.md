# Contributing

Thanks for helping. Bug fixes, tests and better documentation are as
welcome as new features.

## Before you start

- You need your own copy of Fallout to run or test the game. Never add
  game files (`.DAT`, anything from `DATA`) to the repository.
- Read `COOP.md` (what players see) and `docs/coop/ARCHITECTURE.md` (how
  it works). `docs/coop/TESTING.md` explains the tests.
- The project is under the Sustainable Use License, like Fallout CE it is
  based on: free, non-commercial use only. Your contributions fall under
  the same license.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Linux needs SDL2 (`libsdl2-dev`). Release packages for Linux and Windows
are cross-built with zig: `tools/build-release.sh`.

## Making a change

1. With co-op off, the game must play exactly like Fallout CE. If you touch
   engine code, run the single-player comparison (`tools/regress.sh run`
   with upstream and with your build, then `compare`). Mark engine bug
   fixes with a `// CE:` comment that says what was wrong.
2. Add or extend a test script for what you changed, if it can be tested
   (`src/game/coop_autotest.cc`). Run the network tests that are close to
   your change, and the tour (`COOP_AUTOTEST_TOUR=ALL`) for anything that
   touches maps, combat or the network.
3. Keep older versions able to play with yours: network messages only get
   new fields at the end, and new message types must be safe to ignore.
4. Write code like the code around it. Comments explain why, in plain
   English, for someone new to the code.

## Commits

Small commits with a clear message: a short first line, then what was
wrong and how it is fixed. One topic per commit.

## Reporting bugs

The in-game REPORT BUG button sends the details a fix needs (version,
recent log) to the co-op server the game is set up with. In an issue,
please say what happened, what you expected, both players' versions and
operating systems, and whether you played on a LAN or online.
