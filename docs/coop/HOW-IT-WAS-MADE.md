# How this mod was made

This mod was written with an AI coding assistant (Claude, by Anthropic)
working in the repository: reading the engine, writing the code and the
tests, running them, and fixing what they found. A person decided what to
build, tried it, and set the priorities. This document describes how that
worked, the main requests that shaped the mod, and the design decisions
behind it, so that others can follow the reasoning and continue the work.

## The way of working

- **Tests before trust.** Early on a headless test harness was added: the
  game plays itself with scripted input, two processes play over
  localhost, and every map of the game can be visited with two players.
  Most bugs in this mod were found this way, not by playing.
- **Single player is sacred.** With co-op off the game is compared byte
  for byte against upstream Fallout CE, so the mod can't quietly change
  the normal game.
- **Small, explained commits.** Each change says what was wrong and why
  the fix is right.
- **Ship, then listen.** After the first release, player feedback (comments,
  in-game bug reports, server statistics) decided what came next.
- **Round-the-clock testing.** A soak test plays random scenarios at night
  (screen sizes, slow lines, the address sanitizer, a Windows player under
  Wine). It found bugs no one would have reproduced by hand: a frame
  timer that could freeze the game for weeks, a heap overflow in the
  original combat code.

## The requests that shaped it

Paraphrased, roughly in order:

1. *Make Fallout 1 playable by two people, each with their own character,
   behind a setting so single player stays the same.*
2. *Put it in the game's own main menu, with the original look, so nobody
   needs command lines.*
3. *Test it thoroughly: people must be able to finish the game.* (This led
   to the tour of every map and the ending test.)
4. *Find the player's Fallout installation automatically.* (The setup
   terminal.)
5. *Let players update the mod and report bugs from inside the game.*
6. *Add a server browser.* Later: *make online play work without opening
   ports* (the relay) and *without router settings at all*.
7. *Colorblind settings,* then *more color choices,* because there are
   several kinds of colorblindness.
8. *Higher resolutions,* then *mixed screen sizes between the players.*
9. *Chat for online play.*
10. *Find out why players drop out.* (The answer was delay on slow lines,
    which led to flow control, scaled and packed frames.)
11. *Crash reports,* so crashes that players never report still get fixed.
12. *Prepare the code for others: English, documented, and pointing at no
    one's private server.*

## Design decisions

**One game, a thin client.** Player 2's computer runs no game logic. It
sends input and shows pictures. Two copies of a 1997 engine kept in sync
over a network would drift apart in countless ways (random numbers,
scripts, timing); with one game there is nothing to keep in sync. The
cost is bandwidth, which the frame format and flow control keep small.

**Per-player state swapped into the engine.** The engine assumes one
player everywhere. Instead of rewriting it, each player's state is
swapped in when their code runs (`ActivePlayerScope`). Most of the engine
stays untouched, and the question for every feature becomes "whose scope
does this run in?".

**Player 1 talks and travels.** Conversations, the world map and exit
grids belong to player 1. Splitting the party across maps or
conversations would break many scripts. Player 2 trades on their own,
because that touches only their own inventory.

**Shared consequences.** Karma and reputation are shared: it is one game.
If either player dies, the game is lost, as in Fallout.

**Pausing.** When either player is in a menu or it's their combat turn,
the game waits. It keeps the rules of a turn-based game intact.

**Compatibility over cleverness.** Network messages only grow at the
end, so players on different versions can still play together.

**Nothing phones home by default.** The source has no server built in.
Releases can be built with one (`-DCOOP_SERVER`), and each player can
choose their own in `fallout.cfg`. Crash reports are only sent when the
player says yes.

**The menu look.** New screens reuse the game's own buttons and fonts,
and the setup terminal uses a Pip-Boy-style green terminal, so the mod
feels like part of Fallout.

## Bugs worth knowing about

Some of what the tests found, because it shows where this engine bites:

- A player out of sight of a fight sat it out and couldn't move.
- Scripted items in player 2's pack corrupted the heap on map changes.
- Clang (the Windows build) turned undefined behaviour in the original
  combat AI into a crash in the first fight.
- Quest items carried by player 2 were invisible to the scripts.
- Player 2 never got radiation sickness, because the engine checked only
  "the" player.
- The frame limiter read the clock twice; a pause between the reads made
  it sleep for weeks.
- The combat log asked a door for its gender: reading critter data from
  an object's smaller record.

Each has a test or a comment where it was fixed.
