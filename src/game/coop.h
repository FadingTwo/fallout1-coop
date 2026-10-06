#ifndef FALLOUT_GAME_COOP_H_
#define FALLOUT_GAME_COOP_H_

#include "game/critter.h"
#include "game/intface.h"
#include "game/object_types.h"
#include "game/perk_defs.h"
#include "game/proto_types.h"
#include "game/skill_defs.h"
#include "game/stat_defs.h"
#include "game/trait.h"
#include "plib/db/db.h"

namespace fallout {

#define COOP_MAX_PLAYERS 2

// Co-op version, for the update check and bug reports.
#define COOP_VERSION "1.5.9"

// fallout.cfg [coop] server=http://host[:port] - where UPDATE looks for
// new versions (<server>/fallout-coop/latest.txt) and REPORT BUG sends
// reports (<server>/fallout-coop/report).
#define COOP_CONFIG_SERVER_KEY "server"
#define COOP_CONFIG_COLORBLIND_KEY "colorblind"
#define COOP_CONFIG_OUTLINE1_KEY "outline_player1"
#define COOP_CONFIG_OUTLINE2_KEY "outline_player2"

// Player 2's critter proto id. Index 0x3FFF is far beyond critters.lst, so it
// never clashes with a real proto (or with a party member's derived object
// id, which is 18000 + index).
#define COOP_PLAYER2_PID 0x01003FFF

// Drug addictions live in global variables that scripts read and change
// (GVAR_NUKA_COLA_ADDICT .. GVAR_ALCOHOL_ADDICT). They are per player: the
// active player's values are in the globals, the others' in PlayerState.
#define COOP_ADDICTION_COUNT 6

// fallout.cfg: [coop]. A missing section means co-op is disabled, and
// nothing is ever written back, so single-player configs stay untouched.
#define COOP_CONFIG_KEY "coop"
#define COOP_CONFIG_ENABLED_KEY "enabled"
#define COOP_CONFIG_SELFTEST_KEY "selftest"

// Seconds a player may spend on a combat turn; 0 means no limit.
#define COOP_CONFIG_TURN_TIME_KEY "turn_time"

// Everything that makes a player character "theirs". Before co-op this lived
// in per-module globals implicitly owned by `obj_dude`; those modules now
// read and write the active player's copy.
//
// Karma and reputation are deliberately NOT here - they are shared between
// players (see `stat.cc`).
typedef struct PlayerState {
    // The player's critter. For player 1 this mirrors `obj_dude` while no
    // `ActivePlayerScope` is active.
    Object* obj;

    // stat.cc - PC_STAT_REPUTATION and PC_STAT_KARMA slots are unused.
    int pcStats[PC_STAT_COUNT];

    // perk.cc
    int perkLevels[PERK_COUNT];

    // trait.cc
    int traits[PC_TRAIT_MAX];

    // skill.cc
    int taggedSkills[NUM_TAGGED_SKILLS];
    int skillUses[SKILL_COUNT][SKILLS_MAX_USES_PER_DAY];

    // critter.cc
    char name[DUDE_NAME_MAX_LENGTH];
    int killCounts[KILL_TYPE_COUNT];
    int sneakWorking;
    int oldRadLevel;

    // editor.cc
    int lastLevel;
    unsigned char freePerk;

    // intface.cc
    int currentHand;
    InterfaceItemState hands[HAND_COUNT];

    // item.cc - drug withdrawal tracking.
    int withdrawalOnset;
    Object* withdrawalObj;
    int withdrawalGvar;

    // art.cc - the player's own look when not wearing armor.
    int vaultGuyNum;

    // inventry.cc - whose inventory the inventory screen works on.
    Object* inventoryOwner;
    int inventoryPid;

    // Addiction global variables while this player is not active.
    int addictions[COOP_ADDICTION_COUNT];

    // Look when not wearing armor (CoopLook).
    int look;
} PlayerState;

// What a player wears when not wearing armor. Player 1 is always the vault
// jumpsuit, as in the original game.
typedef enum CoopLook {
    COOP_LOOK_VAULT_JUMPSUIT,
    COOP_LOOK_LEATHER_JACKET,
    COOP_LOOK_ROBE,
    COOP_LOOK_COUNT,
} CoopLook;

extern PlayerState* coop_active;

void coop_init();
bool coop_is_enabled();

// Turns co-op on or off for this session (the multiplayer menu); not saved.
void coop_set_enabled(bool enabled);
bool coop_selftest_enabled();

// The player whose data the per-player globals currently refer to.
inline PlayerState* coop_active_player()
{
    return coop_active;
}

int coop_player_count();
PlayerState* coop_player(int index);

// Returns the player state owned by `obj`, or NULL if `obj` is not a player
// character. In single-player this is exactly `obj == obj_dude`.
PlayerState* coop_player_of(Object* obj);

// Returns the player whose critter has proto id `pid`, or NULL.
PlayerState* coop_player_by_pid(int pid);

// True when player 1 is the active player. Player 1 owns everything shared
// (karma, reputation) and the decisions only player 1 may make.
bool coop_primary_is_active();

// The player in control outside of any scope - the one the local mouse and
// keyboard drive. Always player 1 unless switched with
// coop_set_controlled().
PlayerState* coop_controlled_player();

// Hands local control to `player` (debug / local testing). Only allowed
// outside of scopes. Returns false if refused.
bool coop_set_controlled(PlayerState* player);

// Adds player 2 next to player 1, created from the premade character file
// `premadePath` (e.g. "premade\\stealth.gcd"). Returns NULL on failure.
PlayerState* coop_add_player(const char* premadePath);

// Forgets player 2 and returns control to player 1. Called on game reset.
void coop_reset();

// Lets player 2 join through the character creation screen and look
// choice, run in player 2's scope (so a remote player 2 drives them).
// Returns NULL if cancelled.
PlayerState* coop_create_player2();

// Debug key (F9) for local testing: the first press lets player 2 join
// through the character creation screen, later presses switch local
// control between the players.
void coop_handle_switch_key();

// "Global" perks and traits (e.g. Friendly Foe, Jinxed) apply when any
// player has them.
bool coop_any_player_has_perk(int perk);
bool coop_any_player_has_trait(int trait);

// Critter art index of the active player's unarmored look for `gender`.
int coop_look_art(int gender);

// Trading for players other than player 1, who cannot talk (conversations
// are player 1's): reaching a merchant opens the barter screen directly.
bool coop_can_barter_with(Object* critter);
void coop_request_barter(PlayerState* player, Object* merchant);

// Handles pending requests (barter); called by the main game loop.
void coop_process_requests();

// Outline every other player is drawn with (OUTLINE_TYPE_COOP_*), so the
// players can tell each other apart whatever they wear. 0 for anything
// else, and for the viewer.
int coop_outline_type(Object* obj);

// Outline colors, chosen by each player for their own screen (for
// colorblind players, or taste): player 1's (and one PC's) from [coop]
// outline_player1/outline_player2, player 2's sent by their client.
// Without those, [coop] colorblind=1 means white and orange.
#define COOP_OUTLINE_COLORS 6
const char* coop_outline_color_name(int color);
void coop_outline_colors_from_config(int colors[2]);
void coop_set_outline_colors(int playerIndex, const int colors[2]);
// The palette color the viewer draws `player`'s (0 or 1) outline with.
int coop_viewer_outline_color(int player);

// The player whose view is being drawn: the controlled player, except
// while the co-op host renders player 2's view (NULL = controlled player).
void coop_set_viewer(PlayerState* player);

// True if any player character has one of the `results` flags (DAM_*).
bool coop_any_player_has(int results);

// Combat: true once a human turn started at `turnStart` (get_time()) has
// used up [coop] turn_time.
bool coop_turn_time_expired(unsigned int turnStart);

// [coop] turn_time in milliseconds; 0 means no limit.
unsigned int coop_turn_time_limit();

// Lets automated tests play human combat turns: the hook runs at the start
// of a player's turn; if it returns true the turn ends, otherwise the turn
// is played as usual.
typedef bool(CoopTurnHook)(Object* player);
void coop_set_turn_hook(CoopTurnHook* hook);
bool coop_run_turn_hook(Object* player);

// Save game support. The co-op block is appended after all regular save
// data and only written when there is more than one player, so
// single-player saves are unchanged. coop_load() treats a missing block
// (older or single-player saves) as "no extra players".
int coop_save(DB_FILE* stream);
int coop_load(DB_FILE* stream);

int coop_scope_depth();

// Restores scopes down to `depth` without running their destructors. Only
// for code that bypasses C++ unwinding (the script interpreter's longjmp).
void coop_scope_unwind_to(int depth);

// Makes `player` the active player - per-player globals and `obj_dude` -
// for the lifetime of the scope, and always switches back. Nested scopes are
// fine. This is the only place where the active player changes.
//
// Entering a scope for the already active player has no side effects.
class ActivePlayerScope {
public:
    explicit ActivePlayerScope(PlayerState* player);
    ~ActivePlayerScope();

    ActivePlayerScope(const ActivePlayerScope&) = delete;
    ActivePlayerScope& operator=(const ActivePlayerScope&) = delete;
};

// Returns the number of failed checks.
int coop_selftest();

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_H_ */
