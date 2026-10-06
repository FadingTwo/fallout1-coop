#ifndef FALLOUT_GAME_MAIN_H_
#define FALLOUT_GAME_MAIN_H_

namespace fallout {

extern int main_game_paused;

int gnw_main(int argc, char** argv);

// Tests: the game's main loop until it ends; whether it ended in death
// (the death scene would follow).
bool main_game_loop_for_test();

} // namespace fallout

#endif /* FALLOUT_GAME_MAIN_H_ */
