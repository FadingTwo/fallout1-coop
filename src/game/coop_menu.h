#ifndef FALLOUT_GAME_COOP_MENU_H_
#define FALLOUT_GAME_COOP_MENU_H_

namespace fallout {

// The main menu's MULTIPLAYER: host, join or play on one PC. Returns
// MAIN_MENU_NEW_GAME or MAIN_MENU_LOAD_GAME to continue with that game in
// co-op, or -1 to go back to the main menu (also after playing as player 2).
int coop_menu_run();

// After a game started from coop_menu_run(): back to single-player.
void coop_menu_end_session();

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_MENU_H_ */
