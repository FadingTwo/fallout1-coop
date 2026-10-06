#ifndef FALLOUT_GAME_COOP_UPDATE_H_
#define FALLOUT_GAME_COOP_UPDATE_H_

#include <string>

namespace fallout {

// Self-update for the multiplayer menu's UPDATE (see coop_menu.cc).

// Remembers the command line, to restart the same way. Call at startup.
void coop_update_init(int argc, char** argv);

// Removes what an earlier update left behind (replaced files). Call at
// startup.
void coop_update_cleanup();

// Lower case hex SHA-256 of `data`.
std::string coop_update_sha256(const std::string& data);

// Unpacks the package at `packagePath` (.zip or .tar.gz, with the game's
// files in one top folder) over the game's folder. The running executable
// is renamed out of the way first, so this works on Windows too.
bool coop_update_install(const std::string& packagePath, std::string* error);

// Starts the game again with the same command line and exits.
[[noreturn]] void coop_update_restart();

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_UPDATE_H_ */
