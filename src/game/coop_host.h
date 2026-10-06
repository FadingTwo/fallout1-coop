#ifndef FALLOUT_GAME_COOP_HOST_H_
#define FALLOUT_GAME_COOP_HOST_H_

namespace fallout {

// Co-op host (see coop_net.h): plays player 2 from the client's input and
// renders player 2's own view for it.
//
// Free roam (the main game loop): every frame, player 2's input is played
// in player 2's scope with player 2's camera, mouse and cursor mode, then
// player 2's view is rendered and sent, and player 1's view is restored.
//
// Menus and combat (any modal loop): the screen is shared. The active
// player (whose menu or turn it is) drives the input - for player 2 that is
// the client's input - and the other player watches with a status line.

// Starts hosting when [coop] mode=host. Called once after game init.
void coop_host_init();
void coop_host_exit();

bool coop_host_active();

// Main game loop: mark the loop's own get_input() call, so the client's
// input is kept for player 2's turn of the frame.
void coop_host_begin_main_input();
void coop_host_end_main_input();

// Main game loop, before renderPresent(): player 2's part of the frame.
void coop_host_frame();

// Chat while playing over the network: T starts typing for the active
// player, Enter sends ("Name: text" in the message window), Esc cancels.
// Returns true when the key was used for chat (also every key typed).
bool coop_host_chat_key(int keyCode);

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_HOST_H_ */
