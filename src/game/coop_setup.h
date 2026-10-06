#ifndef FALLOUT_GAME_COOP_SETUP_H_
#define FALLOUT_GAME_COOP_SETUP_H_

namespace fallout {

// First run: when the game data (MASTER.DAT, CRITTER.DAT) cannot be found
// with the current fallout.cfg, shows a setup screen - a Pip-Boy style
// terminal, as the game's own art is not available yet - that finds or
// asks for the Fallout installation and writes its paths to fallout.cfg.
//
// Call before the game initializes. Returns false if the player quit.
//
// Testing aids (environment): FALLOUT_SETUP_SEARCH adds folders to look in
// (separated by ';'), FALLOUT_SETUP_SCREENSHOT saves the first frame as a
// BMP, FALLOUT_SETUP_CHOOSE picks the n-th found installation.
bool coop_setup_ensure_game_data(int argc, char** argv);

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_SETUP_H_ */
