#ifndef FALLOUT_GAME_AUTOTEST_H_
#define FALLOUT_GAME_AUTOTEST_H_

namespace fallout {

// Automated, input-free test runs, selected on the command line:
//
//   fallout-ce "[autotest]script=regress"
//
// Scripts:
//   regress - new game, scripted changes, save/load round trip, saves.
//   load    - loads the save `regress` left in slot A.
//
// Results go to ./autotest/ (state dumps, result.txt); the process exits
// with 0 on success and 1 on failure. Combine with SDL_VIDEODRIVER=offscreen
// and SDL_AUDIODRIVER=dummy to run headless.

#define AUTOTEST_CONFIG_KEY "autotest"
#define AUTOTEST_CONFIG_SCRIPT_KEY "script"

typedef int(AutotestNewGameProc)(char* mapFileName);

// Returns a non-zero count of failures.
typedef int(AutotestCheckProc)();

// A test script; report problems with autotest_fail().
typedef int(AutotestScriptProc)(AutotestNewGameProc* newGame);

bool autotest_requested();

// Extra checks run by scripts at a point where a game is loaded.
void autotest_add_check(const char* name, AutotestCheckProc* proc);

// Registers an additional script, selectable as [autotest]script=<name>.
void autotest_add_script(const char* name, AutotestScriptProc* proc);

// Runs the requested script and returns the process exit code.
int autotest_run(AutotestNewGameProc* newGame);

// Helpers for scripts.
void autotest_log(const char* format, ...);
void autotest_fail(const char* format, ...);
void autotest_run_checks(const char* when);

// Dumps the active player character's state to ./autotest/<fileName>.
bool autotest_dump_state(const char* fileName);

bool autotest_files_equal(const char* path1, const char* path2);

// Starts a new game on V13Ent.map with a fixed seed and `premade` as the
// player character.
bool autotest_new_game(AutotestNewGameProc* newGame, const char* premade);

} // namespace fallout

#endif /* FALLOUT_GAME_AUTOTEST_H_ */
