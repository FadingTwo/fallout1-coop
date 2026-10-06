#ifndef FALLOUT_GAME_COOP_CRASH_H_
#define FALLOUT_GAME_COOP_CRASH_H_

#include <string>

namespace fallout {

// Crash reports: when the game crashes, what is known (version, the kind
// of crash, where, and the recent log) goes to crash-report.txt next to
// the game, and the multiplayer menu offers to send it next time.

// Call once, early: where reports go, and whether to catch crashes (a
// pending report is still offered without).
void coop_crash_install(const char* path, bool catchCrashes);

// The report the last run left behind (true), if any.
bool coop_crash_pending(std::string* report);

// The report was sent or declined: kept as crash-report-old.txt.
void coop_crash_done();

// A crash on purpose, for testing (FALLOUT_COOP_CRASH_TEST=crash).
void coop_crash_now();

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_CRASH_H_ */
