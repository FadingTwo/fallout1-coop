#include "game/coop.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include "game/art.h"
#include "game/autotest.h"
#include "game/combat.h"
#include "game/config.h"
#include "game/coop_autotest.h"
#include "game/coop_net.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/editor.h"
#include "game/game.h"
#include "game/game_vars.h"
#include "game/gconfig.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/trait.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/input.h"
#include "plib/gnw/intrface.h"

namespace fallout {

#define COOP_SCOPE_STACK_MAX 64

// "COOP"
#define COOP_SAVE_MAGIC 0x434F4F50
#define COOP_SAVE_VERSION 1

static const int coop_addiction_gvars[COOP_ADDICTION_COUNT] = {
    GVAR_NUKA_COLA_ADDICT,
    GVAR_BUFF_OUT_ADDICT,
    GVAR_MENTATS_ADDICT,
    GVAR_PSYCHO_ADDICT,
    GVAR_RADAWAY_ADDICT,
    GVAR_ALCOHOL_ADDICT,
};

// Zero-initialized player state must match the old `itemCurrentItem = HAND_LEFT`.
static_assert(HAND_LEFT == 0, "HAND_LEFT must be zero");

typedef struct ActivePlayerScopeFrame {
    PlayerState* player;
    Object* dude;
    // Scope entered for the already active player - nothing was changed.
    bool noop;
} ActivePlayerScopeFrame;

static void coop_scope_push(PlayerState* player);
static void coop_scope_pop();
static void coop_sync_base();
static void coop_switch_active(PlayerState* player);
static bool coop_choose_look();

// Zero-initialized, matching the per-module globals this replaces.
static PlayerState coop_players[COOP_MAX_PLAYERS];

static int coop_players_count = 1;

PlayerState* coop_active = &(coop_players[0]);

static ActivePlayerScopeFrame coop_scope_stack[COOP_SCOPE_STACK_MAX];

static int coop_scope_stack_depth = 0;

static bool coop_enabled = false;

static bool coop_selftest_on = false;

static unsigned int coop_turn_time_ms = 0;
static int coop_outline_for[COOP_MAX_PLAYERS][2];

static CoopTurnHook* coop_turn_hook = NULL;

// See coop_set_viewer().
static PlayerState* coop_viewer = NULL;

// Pending barter request (see coop_request_barter()).
static PlayerState* coop_barter_player = NULL;
static Object* coop_barter_merchant = NULL;

void coop_init()
{
    int value;

    coop_enabled = config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_ENABLED_KEY, &value) && value != 0;
    coop_selftest_on = coop_enabled
        && config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_SELFTEST_KEY, &value)
        && value != 0;

    if (coop_enabled && config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_TURN_TIME_KEY, &value) && value > 0) {
        coop_turn_time_ms = (unsigned int)value * 1000;
    }

    int outlines[2];
    coop_outline_colors_from_config(outlines);
    for (int index = 0; index < COOP_MAX_PLAYERS; index++) {
        coop_set_outline_colors(index, outlines);
    }

    if (coop_enabled) {
        debug_printf("\nCOOP: enabled (selftest: %d, turn time: %u ms)\n", coop_selftest_on, coop_turn_time_ms);
    }

    if (coop_selftest_on) {
        autotest_add_check("coop_selftest", coop_selftest);
    }

    coop_net_init();

    coop_autotest_register();
}

bool coop_is_enabled()
{
    return coop_enabled;
}

void coop_set_enabled(bool enabled)
{
    coop_enabled = enabled;
}

bool coop_selftest_enabled()
{
    return coop_selftest_on;
}

int coop_player_count()
{
    return coop_players_count;
}

PlayerState* coop_player(int index)
{
    if (index < 0 || index >= coop_players_count) {
        return NULL;
    }

    return &(coop_players[index]);
}

// Makes `player` active. Per-player data reached through `coop_active`
// switches with the pointer; the addiction globals are copied.
static void coop_switch_active(PlayerState* player)
{
    if (player == coop_active) {
        return;
    }

    if (game_global_vars != NULL) {
        for (int index = 0; index < COOP_ADDICTION_COUNT; index++) {
            coop_active->addictions[index] = game_global_vars[coop_addiction_gvars[index]];
            game_global_vars[coop_addiction_gvars[index]] = player->addictions[index];
        }
    }

    coop_active = player;
}

// Outside of scopes `obj_dude` is the controlled player's critter. The
// engine may (re)create `obj_dude`, so pick it up from there.
static void coop_sync_base()
{
    if (coop_scope_stack_depth == 0) {
        coop_active->obj = obj_dude;
    }
}

PlayerState* coop_player_of(Object* obj)
{
    if (obj == NULL) {
        return NULL;
    }

    if (obj == obj_dude) {
        return coop_active;
    }

    coop_sync_base();

    for (int index = 0; index < coop_players_count; index++) {
        if (coop_players[index].obj == obj) {
            return &(coop_players[index]);
        }
    }

    return NULL;
}

PlayerState* coop_player_by_pid(int pid)
{
    if (pid == 0x1000000) {
        return &(coop_players[0]);
    }

    if (pid == COOP_PLAYER2_PID && coop_players_count > 1) {
        return &(coop_players[1]);
    }

    return NULL;
}

bool coop_primary_is_active()
{
    return coop_active == &(coop_players[0]);
}

PlayerState* coop_controlled_player()
{
    if (coop_scope_stack_depth == 0) {
        return coop_active;
    }

    return coop_scope_stack[0].player;
}

bool coop_set_controlled(PlayerState* player)
{
    if (player == NULL || player->obj == NULL || coop_scope_stack_depth != 0) {
        return false;
    }

    coop_sync_base();

    if (player == coop_active) {
        return true;
    }

    coop_switch_active(player);
    obj_dude = player->obj;

    tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
    intface_update_items(false);
    intface_redraw();
    gmouse_3d_refresh();

    return true;
}

PlayerState* coop_add_player(const char* premadePath)
{
    if (coop_players_count >= COOP_MAX_PLAYERS || obj_dude == NULL || coop_scope_stack_depth != 0) {
        return NULL;
    }

    coop_sync_base();
    Object* leader = coop_players[0].obj;

    proto_player2_reset();

    Object* obj;
    if (obj_new(&obj, art_id(OBJ_TYPE_CRITTER, art_vault_person_nums[GENDER_MALE], 0, 0, 0), COOP_PLAYER2_PID) == -1) {
        return NULL;
    }

    PlayerState* player = &(coop_players[coop_players_count]);
    memset(player, 0, sizeof(*player));
    player->obj = obj;
    coop_players_count++;

    {
        // Everything player-specific is (re)initialized in player 2's scope,
        // exactly as a new game does it for player 1.
        ActivePlayerScope scope(player);
        if (proto_dude_init(premadePath) != 0) {
            debug_printf("\nCOOP: failed to load %s for player 2\n", premadePath);
        }
    }

    obj->flags |= OBJECT_LIGHT_THRU;
    obj_set_light(obj, 4, 0x10000, NULL);
    obj_attempt_placement(obj, leader->tile, leader->elevation, 2);
    obj_set_rotation(obj, leader->rotation, NULL);

    if (partyMemberAdd(obj) == -1) {
        debug_printf("\nCOOP: could not add player 2 to the party\n");
    }

    debug_printf("\nCOOP: added player 2 '%s'\n", player->name);

    return player;
}

static void coop_remove_extra_players()
{
    for (int index = 1; index < coop_players_count; index++) {
        Object* obj = coop_players[index].obj;
        if (obj != NULL) {
            partyMemberRemove(obj);
            obj_erase_object(obj, NULL);
        }
        memset(&(coop_players[index]), 0, sizeof(coop_players[index]));
    }

    coop_players_count = 1;
}

void coop_reset()
{
    coop_scope_unwind_to(0);

    // A reset starts over: no need to keep the other player's addictions.
    if (coop_active != &(coop_players[0])) {
        coop_active = &(coop_players[0]);
        obj_dude = coop_players[0].obj;
    }

    coop_remove_extra_players();

    coop_barter_player = NULL;
    coop_barter_merchant = NULL;
}

static void coop_display_message(const char* format, ...)
{
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    display_print(message);
}

PlayerState* coop_create_player2()
{
    if (coop_players_count != 1 || isInCombat()) {
        return NULL;
    }

    PlayerState* player = coop_add_player("premade\\player.gcd");
    if (player == NULL) {
        return NULL;
    }

    bool created;
    {
        ActivePlayerScope scope(player);
        ResetPlayer();
        created = editor_design(true) == 0;
        if (created) {
            // The editor fades to black when done; show the map again so
            // the look list isn't picked in the dark.
            tile_refresh_display();
            palette_fade_to(cmap);
            coop_choose_look();
            proto_dude_update_gender();
            stat_recalc_derived(obj_dude);
            critter_adjust_hits(obj_dude, 10000);
        }
    }

    if (!created) {
        coop_remove_extra_players();
        tile_refresh_display();
        return NULL;
    }

    // Finishing character creation fades to black (a new game would
    // continue with the intro); come back to the map.
    tile_refresh_display();
    palette_fade_to(cmap);
    coop_display_message("%s joins the game.", player->name);
    return player;
}

void coop_handle_switch_key()
{
    // Over the network player 2 joins from the client and is theirs to
    // control.
    if (!coop_enabled || coop_net_mode() != COOP_NET_NONE) {
        return;
    }

    if (isInCombat()) {
        // In combat, each player's turn takes control automatically.
        coop_display_message("Players take turns in combat.");
        return;
    }

    if (coop_players_count == 1) {
        coop_create_player2();
        return;
    }

    PlayerState* next = coop_controlled_player() == &(coop_players[0]) ? &(coop_players[1]) : &(coop_players[0]);
    if (coop_set_controlled(next)) {
        coop_display_message("Now controlling %s.", next->name);
    }
}

int coop_scope_depth()
{
    return coop_scope_stack_depth;
}

void coop_scope_unwind_to(int depth)
{
    while (coop_scope_stack_depth > depth) {
        coop_scope_pop();
    }
}

static void coop_scope_push(PlayerState* player)
{
    if (coop_scope_stack_depth >= COOP_SCOPE_STACK_MAX) {
        // Unbounded nesting is a bug; refusing to switch is the safest thing
        // we can do without exceptions.
        debug_printf("\nCOOP: ActivePlayerScope stack overflow!\n");
        coop_scope_stack_depth++;
        return;
    }

    coop_sync_base();

    ActivePlayerScopeFrame* frame = &(coop_scope_stack[coop_scope_stack_depth++]);
    frame->player = coop_active;
    frame->dude = obj_dude;
    frame->noop = player == NULL || player == coop_active;

    if (!frame->noop) {
        coop_switch_active(player);
        if (player->obj != NULL) {
            obj_dude = player->obj;
        }
    }
}

static void coop_scope_pop()
{
    if (coop_scope_stack_depth <= 0) {
        debug_printf("\nCOOP: ActivePlayerScope stack underflow!\n");
        return;
    }

    coop_scope_stack_depth--;
    if (coop_scope_stack_depth >= COOP_SCOPE_STACK_MAX) {
        return;
    }

    ActivePlayerScopeFrame* frame = &(coop_scope_stack[coop_scope_stack_depth]);
    if (!frame->noop) {
        coop_switch_active(frame->player);
        obj_dude = frame->dude;
    }
}

ActivePlayerScope::ActivePlayerScope(PlayerState* player)
{
    coop_scope_push(player);
}

ActivePlayerScope::~ActivePlayerScope()
{
    coop_scope_pop();
}

static int coop_write_player(DB_FILE* stream, PlayerState* player)
{
    Proto* proto;
    if (proto_ptr(player->obj->pid, &proto) == -1) return -1;

    if (db_fwriteInt(stream, player->obj->pid) == -1) return -1;
    if (db_fwriteIntCount(stream, player->pcStats, PC_STAT_COUNT) == -1) return -1;
    if (db_fwriteIntCount(stream, player->perkLevels, PERK_COUNT) == -1) return -1;
    if (db_fwriteIntCount(stream, player->traits, PC_TRAIT_MAX) == -1) return -1;
    if (db_fwriteIntCount(stream, player->taggedSkills, NUM_TAGGED_SKILLS) == -1) return -1;
    if (db_fwriteIntCount(stream, &(player->skillUses[0][0]), SKILL_COUNT * SKILLS_MAX_USES_PER_DAY) == -1) return -1;
    if (db_fwrite(player->name, DUDE_NAME_MAX_LENGTH, 1, stream) != 1) return -1;
    if (db_fwriteIntCount(stream, player->killCounts, KILL_TYPE_COUNT) == -1) return -1;
    if (db_fwriteInt(stream, player->sneakWorking) == -1) return -1;
    if (db_fwriteInt(stream, player->oldRadLevel) == -1) return -1;
    if (db_fwriteInt(stream, player->lastLevel) == -1) return -1;
    if (db_fwriteByte(stream, player->freePerk) == -1) return -1;
    if (db_fwriteInt(stream, player->currentHand) == -1) return -1;
    if (db_fwriteInt(stream, player->vaultGuyNum) == -1) return -1;
    if (db_fwriteIntCount(stream, player->addictions, COOP_ADDICTION_COUNT) == -1) return -1;
    if (db_fwriteInt(stream, player->look) == -1) return -1;
    if (db_fwriteInt(stream, proto->fid) == -1) return -1;
    if (critter_write_data(stream, &(proto->critter.data)) == -1) return -1;

    return 0;
}

static int coop_read_player(DB_FILE* stream, PlayerState* player, int* pidPtr, CritterProtoData* protoData, int* protoFidPtr)
{
    if (db_freadInt(stream, pidPtr) == -1) return -1;
    if (db_freadIntCount(stream, player->pcStats, PC_STAT_COUNT) == -1) return -1;
    if (db_freadIntCount(stream, player->perkLevels, PERK_COUNT) == -1) return -1;
    if (db_freadIntCount(stream, player->traits, PC_TRAIT_MAX) == -1) return -1;
    if (db_freadIntCount(stream, player->taggedSkills, NUM_TAGGED_SKILLS) == -1) return -1;
    if (db_freadIntCount(stream, &(player->skillUses[0][0]), SKILL_COUNT * SKILLS_MAX_USES_PER_DAY) == -1) return -1;
    if (db_fread(player->name, DUDE_NAME_MAX_LENGTH, 1, stream) != 1) return -1;
    if (db_freadIntCount(stream, player->killCounts, KILL_TYPE_COUNT) == -1) return -1;
    if (db_freadInt(stream, &(player->sneakWorking)) == -1) return -1;
    if (db_freadInt(stream, &(player->oldRadLevel)) == -1) return -1;
    if (db_freadInt(stream, &(player->lastLevel)) == -1) return -1;
    if (db_freadByte(stream, &(player->freePerk)) == -1) return -1;
    if (db_freadInt(stream, &(player->currentHand)) == -1) return -1;
    if (db_freadInt(stream, &(player->vaultGuyNum)) == -1) return -1;
    if (db_freadIntCount(stream, player->addictions, COOP_ADDICTION_COUNT) == -1) return -1;
    if (db_freadInt(stream, &(player->look)) == -1) return -1;
    if (db_freadInt(stream, protoFidPtr) == -1) return -1;
    if (critter_read_data(stream, protoData) == -1) return -1;

    return 0;
}

bool coop_any_player_has(int results)
{
    coop_sync_base();

    for (int index = 0; index < coop_players_count; index++) {
        Object* obj = coop_players[index].obj;
        if (obj != NULL && (obj->data.critter.combat.results & results) != 0) {
            return true;
        }
    }

    return false;
}

bool coop_turn_time_expired(unsigned int turnStart)
{
    return coop_turn_time_ms != 0 && elapsed_time(turnStart) >= coop_turn_time_ms;
}

unsigned int coop_turn_time_limit()
{
    return coop_turn_time_ms;
}

void coop_set_turn_hook(CoopTurnHook* hook)
{
    coop_turn_hook = hook;
}

bool coop_run_turn_hook(Object* player)
{
    if (coop_turn_hook == NULL) {
        return false;
    }

    return coop_turn_hook(player);
}

int coop_look_art(int gender)
{
    // Male and female art of each look.
    static const char* lookArt[COOP_LOOK_COUNT][GENDER_COUNT] = {
        { NULL, NULL },
        { "hmmaxx", "hfmaxx" },
        { "harobe", "harobe" },
    };

    int look = coop_active->look;
    if (look > COOP_LOOK_VAULT_JUMPSUIT && look < COOP_LOOK_COUNT) {
        int index = art_critter_index(lookArt[look][gender]);
        if (index != -1) {
            return index;
        }
    }

    return art_vault_person_nums[gender];
}

// Lets the active player pick a look. Returns false when cancelled.
static bool coop_choose_look()
{
    char jumpsuit[] = "Vault jumpsuit";
    char jacket[] = "Leather jacket";
    char robe[] = "Robe";
    char* items[COOP_LOOK_COUNT] = { jumpsuit, jacket, robe };

    int choice = win_list_select("Choose your look", items, COOP_LOOK_COUNT, NULL, 80 + (screenGetWidth() - 640) / 2, 80 + (screenGetHeight() - 480) / 2, 0x10000 | 0x100 | 4);
    if (choice < 0 || choice >= COOP_LOOK_COUNT) {
        return false;
    }

    coop_active->look = choice;
    return true;
}

bool coop_can_barter_with(Object* critter)
{
    Proto* proto;
    return critter != NULL
        && PID_TYPE(critter->pid) == OBJ_TYPE_CRITTER
        && proto_ptr(critter->pid, &proto) != -1
        && (proto->critter.data.flags & CRITTER_BARTER) != 0
        && !critter_is_dead(critter);
}

void coop_request_barter(PlayerState* player, Object* merchant)
{
    coop_barter_player = player;
    coop_barter_merchant = merchant;
}

void coop_process_requests()
{
    if (coop_barter_player == NULL) {
        return;
    }

    PlayerState* player = coop_barter_player;
    Object* merchant = coop_barter_merchant;
    coop_barter_player = NULL;
    coop_barter_merchant = NULL;

    if (coop_player_of(player->obj) != player || !coop_can_barter_with(merchant) || isInCombat()) {
        return;
    }

    // Drawn as the trading player sees it (the dialog screen can show the
    // map around the merchant).
    PlayerState* oldViewer = coop_viewer;
    coop_viewer = player;
    {
        ActivePlayerScope scope(player);
        gdialog_barter(merchant);
    }
    coop_viewer = oldViewer;
}

void coop_set_viewer(PlayerState* player)
{
    coop_viewer = player;
}

int coop_outline_type(Object* obj)
{
    if (coop_players_count <= 1 || obj == NULL || (obj->flags & OBJECT_HIDDEN) != 0) {
        return 0;
    }

    PlayerState* player = coop_player_of(obj);
    PlayerState* viewer = coop_viewer != NULL ? coop_viewer : coop_controlled_player();
    if (player == NULL || player == viewer || critter_is_dead(obj)) {
        return 0;
    }

    return player == &(coop_players[0]) ? OUTLINE_TYPE_COOP_PLAYER1 : OUTLINE_TYPE_COOP_PLAYER2;
}

// Green is Pip-Boy green, yellow close to New Vegas' amber. The palette
// has no bright blue or cyan that does not cycle; blue is a steel blue.
// Red is left out: enemies are outlined red in fights.
static const char* const coop_outline_names[COOP_OUTLINE_COLORS] = {
    "GREEN",
    "YELLOW",
    "WHITE",
    "ORANGE",
    "PINK",
    "BLUE",
};

static const int coop_outline_rgb[COOP_OUTLINE_COLORS] = {
    (4 << 10) | (31 << 5) | 4,
    (31 << 10) | (27 << 5) | 7,
    (31 << 10) | (31 << 5) | 31,
    (31 << 10) | (14 << 5) | 0,
    (31 << 10) | (22 << 5) | 30,
    (12 << 10) | (17 << 5) | 18,
};

const char* coop_outline_color_name(int color)
{
    return color >= 0 && color < COOP_OUTLINE_COLORS ? coop_outline_names[color] : coop_outline_names[0];
}

void coop_outline_colors_from_config(int colors[2])
{
    int colorblind = 0;
    config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_COLORBLIND_KEY, &colorblind);
    colors[0] = colorblind != 0 ? 2 : 0;
    colors[1] = colorblind != 0 ? 3 : 1;

    int value;
    if (config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_OUTLINE1_KEY, &value) && value >= 0 && value < COOP_OUTLINE_COLORS) {
        colors[0] = value;
    }
    if (config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_OUTLINE2_KEY, &value) && value >= 0 && value < COOP_OUTLINE_COLORS) {
        colors[1] = value;
    }
}

void coop_set_outline_colors(int playerIndex, const int colors[2])
{
    if (playerIndex >= 0 && playerIndex < COOP_MAX_PLAYERS) {
        for (int player = 0; player < 2; player++) {
            int color = colors[player];
            coop_outline_for[playerIndex][player] = color >= 0 && color < COOP_OUTLINE_COLORS ? color : player;
        }
    }
}

int coop_viewer_outline_color(int player)
{
    PlayerState* viewer = coop_viewer != NULL ? coop_viewer : coop_controlled_player();
    int index = viewer != NULL ? (int)(viewer - coop_players) : 0;
    if (index < 0 || index >= COOP_MAX_PLAYERS) {
        index = 0;
    }
    return colorTable[coop_outline_rgb[coop_outline_for[index][player != 0 ? 1 : 0]]];
}

bool coop_any_player_has_perk(int perk)
{
    for (int index = 0; index < coop_players_count; index++) {
        ActivePlayerScope scope(&(coop_players[index]));
        if (perk_level(perk) != 0) {
            return true;
        }
    }

    return false;
}

bool coop_any_player_has_trait(int trait)
{
    for (int index = 0; index < coop_players_count; index++) {
        ActivePlayerScope scope(&(coop_players[index]));
        if (trait_level(trait) != 0) {
            return true;
        }
    }

    return false;
}

int coop_save(DB_FILE* stream)
{
    if (coop_players_count <= 1) {
        return 0;
    }

    if (db_fwriteInt(stream, COOP_SAVE_MAGIC) == -1) return -1;
    if (db_fwriteInt(stream, COOP_SAVE_VERSION) == -1) return -1;
    if (db_fwriteInt(stream, coop_players_count - 1) == -1) return -1;

    for (int index = 1; index < coop_players_count; index++) {
        if (coop_write_player(stream, &(coop_players[index])) == -1) {
            return -1;
        }
    }

    return 0;
}

static Object* coop_find_critter_by_pid(int pid)
{
    for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
        if (obj->pid == pid) {
            return obj;
        }
    }

    return NULL;
}

int coop_load(DB_FILE* stream)
{
    int magic;
    if (db_freadInt(stream, &magic) == -1 || magic != COOP_SAVE_MAGIC) {
        // Single-player or pre-co-op save.
        return 0;
    }

    int version;
    if (db_freadInt(stream, &version) == -1) return -1;
    if (version < 1 || version > COOP_SAVE_VERSION) {
        debug_printf("\nCOOP: unsupported save version %d\n", version);
        return -1;
    }

    int extraPlayers;
    if (db_freadInt(stream, &extraPlayers) == -1) return -1;
    if (extraPlayers < 0 || extraPlayers >= COOP_MAX_PLAYERS) {
        debug_printf("\nCOOP: bad player count %d in save\n", extraPlayers);
        return -1;
    }

    for (int index = 0; index < extraPlayers; index++) {
        PlayerState state;
        memset(&state, 0, sizeof(state));

        int pid;
        int protoFid;
        CritterProtoData protoData;
        if (coop_read_player(stream, &state, &pid, &protoData, &protoFid) == -1) {
            return -1;
        }

        if (pid != COOP_PLAYER2_PID) {
            debug_printf("\nCOOP: unexpected player pid %x in save\n", pid);
            return -1;
        }

        Object* obj = coop_find_critter_by_pid(pid);
        if (obj == NULL) {
            debug_printf("\nCOOP: player %d's critter is missing from the map\n", index + 2);
            return -1;
        }

        proto_player2_reset();

        Proto* proto;
        proto_ptr(pid, &proto);
        proto->fid = protoFid;
        critter_copy(&(proto->critter.data), &protoData);

        PlayerState* player = &(coop_players[coop_players_count++]);
        memcpy(player, &state, sizeof(*player));
        player->obj = obj;
        player->inventoryOwner = obj;
        player->inventoryPid = pid;

        ActivePlayerScope scope(player);
        stat_recalc_derived(obj);

        debug_printf("\nCOOP: loaded player %d '%s'\n", index + 2, player->name);
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Self-test: exercises scope switching against a throwaway copy of the
// active player. Must leave the game state exactly as it found it.

static int coop_selftest_failures;

static void coop_selftest_check(bool condition, const char* what)
{
    if (!condition) {
        coop_selftest_failures++;
        debug_printf("\nCOOP selftest FAILED: %s\n", what);
    }
}

static void coop_selftest_compare_visible(const PlayerState* expected, const char* when)
{
    char what[128];

    for (int perk = 0; perk < PERK_COUNT; perk++) {
        if (perk_level(perk) != expected->perkLevels[perk]) {
            snprintf(what, sizeof(what), "%s: perk %d", when, perk);
            coop_selftest_check(false, what);
        }
    }

    for (int trait = 0; trait < TRAIT_COUNT; trait++) {
        bool expectedLevel = expected->traits[0] == trait || expected->traits[1] == trait;
        if ((trait_level(trait) != 0) != expectedLevel) {
            snprintf(what, sizeof(what), "%s: trait %d", when, trait);
            coop_selftest_check(false, what);
        }
    }

    for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
        if (pcStat == PC_STAT_REPUTATION || pcStat == PC_STAT_KARMA) {
            continue;
        }

        if (stat_pc_get(pcStat) != expected->pcStats[pcStat]) {
            snprintf(what, sizeof(what), "%s: pc stat %d", when, pcStat);
            coop_selftest_check(false, what);
        }
    }

    int tags[NUM_TAGGED_SKILLS];
    skill_get_tags(tags, NUM_TAGGED_SKILLS);
    snprintf(what, sizeof(what), "%s: tagged skills", when);
    coop_selftest_check(memcmp(tags, expected->taggedSkills, sizeof(tags)) == 0, what);

    for (int killType = 0; killType < KILL_TYPE_COUNT; killType++) {
        if (critter_kill_count(killType) != expected->killCounts[killType]) {
            snprintf(what, sizeof(what), "%s: kill count %d", when, killType);
            coop_selftest_check(false, what);
        }
    }

    snprintf(what, sizeof(what), "%s: name", when);
    coop_selftest_check(strcmp(critter_name(obj_dude), expected->name) == 0, what);

    snprintf(what, sizeof(what), "%s: current hand", when);
    coop_selftest_check((intface_is_item_right_hand() != 0) == (expected->currentHand == HAND_RIGHT), what);
}

int coop_selftest()
{
    coop_selftest_failures = 0;

    if (obj_dude == NULL) {
        return 0;
    }

    int depth = coop_scope_depth();
    Object* dude = obj_dude;
    PlayerState* original = coop_active_player();

    PlayerState before;
    memcpy(&before, original, sizeof(before));

    // Shadow player: a copy of the active one, sharing its critter.
    PlayerState shadow;
    memcpy(&shadow, original, sizeof(shadow));
    shadow.obj = obj_dude;

    coop_selftest_check(coop_player_of(obj_dude) == original, "player_of(obj_dude)");
    coop_selftest_check(coop_player_of(NULL) == NULL, "player_of(NULL)");

    int karma = stat_pc_get(PC_STAT_KARMA);

    {
        ActivePlayerScope scope(&shadow);
        coop_selftest_check(coop_active_player() == &shadow, "scope activates player");
        coop_selftest_check(obj_dude == dude, "obj_dude follows player");
        coop_selftest_compare_visible(&before, "inside scope");

        // Writes must land in the shadow only...
        stat_pc_set(PC_STAT_UNSPENT_SKILL_POINTS, stat_pc_get(PC_STAT_UNSPENT_SKILL_POINTS) + 7);
        critter_kill_count_inc(0);
        critter_pc_set_name("coop-selftest");

        // ...except karma, which is shared.
        stat_pc_set(PC_STAT_KARMA, karma + 1);

        {
            ActivePlayerScope nestedSame(&shadow);
            coop_selftest_check(coop_active_player() == &shadow, "nested same player");
        }
        coop_selftest_check(coop_active_player() == &shadow, "after nested same player");

        {
            ActivePlayerScope nestedOther(original);
            coop_selftest_check(coop_active_player() == original, "nested other player");
            coop_selftest_compare_visible(&before, "nested other player");
        }
        coop_selftest_check(coop_active_player() == &shadow, "after nested other player");
        coop_selftest_check(strcmp(critter_name(obj_dude), "coop-selftest") == 0, "shadow keeps own writes");
    }

    coop_selftest_check(coop_active_player() == original, "scope restores player");
    coop_selftest_check(obj_dude == dude, "scope restores obj_dude");
    coop_selftest_check(stat_pc_get(PC_STAT_KARMA) == karma + 1, "karma is shared");
    stat_pc_set(PC_STAT_KARMA, karma);

    coop_selftest_compare_visible(&before, "after scope");

    // Simulate the interpreter's longjmp: scopes whose destructors never run.
    alignas(ActivePlayerScope) unsigned char storage[2][sizeof(ActivePlayerScope)];
    new (storage[0]) ActivePlayerScope(&shadow);
    new (storage[1]) ActivePlayerScope(original);
    coop_scope_unwind_to(depth);
    coop_selftest_check(coop_active_player() == original, "unwind restores player");
    coop_selftest_check(obj_dude == dude, "unwind restores obj_dude");
    coop_selftest_check(coop_scope_depth() == depth, "unwind restores depth");

    // The first scope ever entered records player 1's critter, and leaving
    // the active role stores the addiction globals; nothing else may change.
    before.obj = original->obj;
    memcpy(before.addictions, original->addictions, sizeof(before.addictions));
    coop_selftest_check(memcmp(original, &before, sizeof(before)) == 0, "player state untouched");

    char message[80];
    if (coop_selftest_failures == 0) {
        snprintf(message, sizeof(message), "Co-op selftest: OK");
    } else {
        snprintf(message, sizeof(message), "Co-op selftest: %d FAILED (see debug log)", coop_selftest_failures);
    }
    debug_printf("\n%s\n", message);
    display_print(message);

    return coop_selftest_failures;
}

} // namespace fallout
