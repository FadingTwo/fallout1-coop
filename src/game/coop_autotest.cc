// Autotest scripts for co-op, see autotest.h. Run e.g. with
//
//   AUTOTEST_SCRIPT=coop tools/regress.sh run <binary> <out> "[coop]enabled=1"

#include "game/coop_autotest.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <chrono>
#include <iterator>
#include <string>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/autotest.h"
#include "game/combat.h"
#include "game/combat_defs.h"
#include "game/coop.h"
#include "game/coop_crash.h"
#include "game/coop_host.h"
#include "game/coop_lobby.h"
#include "game/coop_menu.h"
#include "game/coop_net.h"
#include "game/credits.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/game_vars.h"
#include "game/gconfig.h"
#include "game/endgame.h"
#include "game/gdialog.h"
#include "game/gmovie.h"
#include "game/gsound.h"
#include "game/scripts.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/loadsave.h"
#include "game/main.h"
#include "game/mainmenu.h"
#include "game/map.h"
#include "game/palette.h"
#include "game/party.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/perk.h"
#include "game/proto.h"
#include "game/queue.h"
#include "game/roll.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/inject.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/svga.h"

#include <SDL.h>

namespace fallout {

// Quick save slot used by the co-op scripts (0-based; SLOT08).
#define COOP_AUTOTEST_SLOT 7

// Human combat turns played by a test hook.
static Object* coop_autotest_enemy = NULL;
static int coop_autotest_turns[COOP_MAX_PLAYERS];
static int coop_autotest_turns_total = 0;

static int coop_autotest_players(AutotestNewGameProc* newGame);
static int coop_autotest_travel(AutotestNewGameProc* newGame);
static int coop_autotest_rads(AutotestNewGameProc* newGame);
static int coop_autotest_interact(AutotestNewGameProc* newGame);
static int coop_autotest_map_time(AutotestNewGameProc* newGame);
static int coop_autotest_pack(AutotestNewGameProc* newGame);
static int coop_autotest_story_kill(AutotestNewGameProc* newGame);
static void coop_autotest_log_story(const char* when);
static bool coop_autotest_frames_until(unsigned int ms, bool (*done)());
static void coop_autotest_host_frame();
static bool coop_autotest_players_together(const char* when);
static int coop_autotest_combat(AutotestNewGameProc* newGame);
static bool coop_autotest_attack_turn(Object* player);
static bool coop_autotest_pass_turn(Object* player);
static Object* coop_autotest_spawn_enemy(Object* nextTo);
static int coop_autotest_join(AutotestNewGameProc* newGame);
static int coop_autotest_looks(AutotestNewGameProc* newGame);
static int coop_autotest_barter(AutotestNewGameProc* newGame);
static int coop_autotest_net_host(AutotestNewGameProc* newGame);
static int coop_autotest_net_client(AutotestNewGameProc* newGame);
static void coop_autotest_net_client_exit();
static int coop_autotest_net_play_host(AutotestNewGameProc* newGame);
static int coop_autotest_net_play_client(AutotestNewGameProc* newGame);
static int coop_autotest_net_combat_host(AutotestNewGameProc* newGame);
static int coop_autotest_net_combat_client(AutotestNewGameProc* newGame);
static int coop_autotest_main_menu(AutotestNewGameProc* newGame);
static int coop_autotest_menu_flow(AutotestNewGameProc* newGame);
static void coop_autotest_screenshot();
static void coop_autotest_equip(PlayerState* player, const char* const* names, int count, int hand);
static void coop_autotest_scene();
static int coop_autotest_menu_join(AutotestNewGameProc* newGame);
static int coop_autotest_menu_server(AutotestNewGameProc* newGame);
static int coop_autotest_menu_host_code(AutotestNewGameProc* newGame);
static int coop_autotest_menu_crash_report(AutotestNewGameProc* newGame);
static int coop_autotest_net_tour_host(AutotestNewGameProc* newGame);
static int coop_autotest_net_tour_client(AutotestNewGameProc* newGame);
static int coop_autotest_net_session_host(AutotestNewGameProc* newGame);
static int coop_autotest_net_ending_host(AutotestNewGameProc* newGame);
static int coop_autotest_movie(AutotestNewGameProc* newGame);
static int coop_autotest_menu_more(AutotestNewGameProc* newGame);
static int coop_autotest_credits(AutotestNewGameProc* newGame);
static int coop_autotest_net_death_host(AutotestNewGameProc* newGame);
static void coop_autotest_session_shots()
{
    inject_wait(90);
    inject_call(coop_autotest_screenshot);
    inject_call(coop_autotest_session_shots);
}

static int coop_autotest_net_session_client(AutotestNewGameProc* newGame);
static void coop_autotest_compare_lookups(PlayerState* player, const char* when);
static bool coop_autotest_files_equal_except(const char* path1, const char* path2, const char* skipPrefix);

void coop_autotest_register()
{
    autotest_add_script("coop", coop_autotest_players);
    autotest_add_script("coop_travel", coop_autotest_travel);
    autotest_add_script("coop_rads", coop_autotest_rads);
    autotest_add_script("coop_interact", coop_autotest_interact);
    autotest_add_script("map_time", coop_autotest_map_time);
    autotest_add_script("pack", coop_autotest_pack);
    autotest_add_script("story_kill", coop_autotest_story_kill);
    autotest_add_script("coop_combat", coop_autotest_combat);
    autotest_add_script("coop_join", coop_autotest_join);
    autotest_add_script("coop_looks", coop_autotest_looks);
    autotest_add_script("coop_barter", coop_autotest_barter);
    autotest_add_script("net_host", coop_autotest_net_host);
    autotest_add_script("net_client", coop_autotest_net_client);
    autotest_add_script("net_play_host", coop_autotest_net_play_host);
    autotest_add_script("net_play_client", coop_autotest_net_play_client);
    autotest_add_script("net_combat_host", coop_autotest_net_combat_host);
    autotest_add_script("net_combat_client", coop_autotest_net_combat_client);
    autotest_add_script("main_menu", coop_autotest_main_menu);
    autotest_add_script("menu_flow", coop_autotest_menu_flow);
    autotest_add_script("menu_join", coop_autotest_menu_join);
    autotest_add_script("menu_server", coop_autotest_menu_server);
    autotest_add_script("menu_host_code", coop_autotest_menu_host_code);
    autotest_add_script("menu_crash_report", coop_autotest_menu_crash_report);
    autotest_add_script("net_tour_host", coop_autotest_net_tour_host);
    autotest_add_script("net_tour_client", coop_autotest_net_tour_client);
    autotest_add_script("net_session_host", coop_autotest_net_session_host);
    autotest_add_script("net_session_client", coop_autotest_net_session_client);
    autotest_add_script("net_ending_host", coop_autotest_net_ending_host);
    autotest_add_script("movie", coop_autotest_movie);
    autotest_add_script("menu_more", coop_autotest_menu_more);
    autotest_add_script("credits", coop_autotest_credits);
    autotest_add_script("net_death_host", coop_autotest_net_death_host);
}

// Player 2 is created next to player 1 without touching player 1, keeps
// its own data, and can take local control.
static int coop_autotest_players(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    // Level 4 with room to spare: player 2's XP below must not level up
    // player 1, so only the XP total may differ afterwards.
    stat_pc_add_experience(6500);

    PlayerState* p1 = coop_player(0);
    Object* p1obj = obj_dude;
    int karma = game_get_global_var(GVAR_PLAYER_REPUATION);
    int karmaStat = stat_pc_get(PC_STAT_KARMA);
    int p1Xp = stat_pc_get(PC_STAT_EXPERIENCE);

    autotest_dump_state("p1_before.txt");

    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    Object* p2obj = p2->obj;
    autotest_log("p1 '%s' tile %d, p2 '%s' tile %d, distance %d",
        critter_name(p1obj), p1obj->tile, critter_name(p2obj), p2obj->tile, obj_dist(p1obj, p2obj));

    if (coop_player_count() != 2) autotest_fail("player count %d", coop_player_count());
    if (obj_dude != p1obj) autotest_fail("obj_dude changed by adding player 2");
    if (coop_active_player() != p1) autotest_fail("active player changed by adding player 2");
    if (coop_player_of(p2obj) != p2) autotest_fail("player_of(p2)");
    if (coop_player_of(p1obj) != p1) autotest_fail("player_of(p1)");
    if (coop_player_by_pid(p2obj->pid) != p2) autotest_fail("player_by_pid(p2)");
    if (strcmp(critter_name(p1obj), critter_name(p2obj)) == 0) autotest_fail("players share a name");
    if (p2obj->elevation != p1obj->elevation || obj_dist(p1obj, p2obj) > 5) autotest_fail("player 2 not placed next to player 1");
    if (game_get_global_var(GVAR_PLAYER_REPUATION) != karma || stat_pc_get(PC_STAT_KARMA) != karmaStat) {
        autotest_fail("adding player 2 changed shared karma");
    }

    autotest_dump_state("p1_after_add.txt");
    if (!autotest_files_equal("autotest/p1_before.txt", "autotest/p1_after_add.txt")) {
        autotest_fail("adding player 2 changed player 1");
    }

    {
        ActivePlayerScope scope(p2);
        autotest_dump_state("p2.txt");

        // Player 2 only changes.
        int perksAdded = 0;
        for (int perk = PERK_COUNT - 1; perk >= 0 && perksAdded < 2; perk--) {
            if (perk_add(perk) == 0) {
                autotest_log("p2 perk added: %d", perk);
                perksAdded++;
            }
        }
        critter_kill_count_inc(KILL_TYPE_RAT);
        stat_pc_add_experience(1000);
        skill_inc_point(obj_dude, SKILL_STEAL);
        game_set_global_var(GVAR_NUKA_COLA_ADDICT, 1);
        autotest_dump_state("p2_changed.txt");
    }

    if (autotest_files_equal("autotest/p2.txt", "autotest/p2_changed.txt")) {
        autotest_fail("player 2 changes had no effect");
    }

    // XP is earned by every player; nothing else may leak into player 1.
    if (stat_pc_get(PC_STAT_EXPERIENCE) < p1Xp + 1000) {
        autotest_fail("player 1 did not get player 2's XP (%d -> %d)", p1Xp, stat_pc_get(PC_STAT_EXPERIENCE));
    }

    if (game_get_global_var(GVAR_NUKA_COLA_ADDICT) != 0) {
        autotest_fail("player 2's addiction shows for player 1");
    }

    autotest_dump_state("p1_after_p2_changes.txt");
    if (!coop_autotest_files_equal_except("autotest/p1_before.txt", "autotest/p1_after_p2_changes.txt", "pc_stat 2 ")) {
        autotest_fail("player 2 changes leaked into player 1");
    }

    coop_autotest_compare_lookups(p2, "while player 1 active");

    if (!coop_set_controlled(p2)) {
        autotest_fail("coop_set_controlled(p2) refused");
    } else {
        if (obj_dude != p2obj || coop_active_player() != p2 || coop_controlled_player() != p2) {
            autotest_fail("control switch did not activate player 2");
        }

        autotest_dump_state("p2_controlled.txt");
        if (!autotest_files_equal("autotest/p2_changed.txt", "autotest/p2_controlled.txt")) {
            autotest_fail("player 2 state differs when controlled");
        }

        coop_autotest_compare_lookups(p1, "while player 2 controlled");

        tile_refresh_display();
        dump_screen();

        // Only player 1 saves.
        lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
        if (SaveGame(LOAD_SAVE_MODE_QUICK) == 1) {
            autotest_fail("player 2 was allowed to save");
        }

        coop_set_controlled(p1);
        if (obj_dude != p1obj || coop_active_player() != p1) {
            autotest_fail("control did not return to player 1");
        }
    }

    autotest_run_checks("with two players");

    tile_refresh_display();
    dump_screen();

    // Save/load round trip keeps both players.
    {
        ActivePlayerScope scope(p2);
        autotest_dump_state("p2_before_save.txt");
    }
    autotest_dump_state("p1_before_save.txt");

    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (SaveGame(LOAD_SAVE_MODE_QUICK) != 1) {
        autotest_fail("save failed");
        return -1;
    }

    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (LoadGame(LOAD_SAVE_MODE_QUICK) != 1) {
        autotest_fail("load failed");
        return -1;
    }

    if (coop_player_count() != 2) {
        autotest_fail("player 2 missing after load (count %d)", coop_player_count());
        return -1;
    }

    p2 = coop_player(1);
    autotest_log("after load: p1 '%s', p2 '%s' tile %d", critter_name(obj_dude), critter_name(p2->obj), p2->obj->tile);

    autotest_dump_state("p1_after_load.txt");
    if (!autotest_files_equal("autotest/p1_before_save.txt", "autotest/p1_after_load.txt")) {
        autotest_fail("player 1 differs after load");
    }

    {
        ActivePlayerScope scope(p2);
        autotest_dump_state("p2_after_load.txt");
    }
    if (!autotest_files_equal("autotest/p2_before_save.txt", "autotest/p2_after_load.txt")) {
        autotest_fail("player 2 differs after load");
    }

    coop_autotest_compare_lookups(p2, "after load");
    autotest_run_checks("after load");

    if (game_get_global_var(GVAR_NUKA_COLA_ADDICT) != 0) {
        autotest_fail("player 1 addicted after load");
    }

    {
        ActivePlayerScope scope(p2);
        if (game_get_global_var(GVAR_NUKA_COLA_ADDICT) != 1) {
            autotest_fail("player 2's addiction lost");
        }
    }

    return 0;
}

// Player 2 follows player 1 through map and elevation changes and cannot
// use exit grids alone.
// How long a map change takes (COOP_AUTOTEST_MAP, default BROHD34.MAP),
// without co-op: to compare screen sizes and builds.
static int coop_autotest_map_time(AutotestNewGameProc* newGame)
{
    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    const char* name = getenv("COOP_AUTOTEST_MAP");
    MapTransition transition;
    transition.map = map_match_map_name(name != NULL ? name : "BROHD34.MAP");
    transition.elevation = 0;
    transition.tile = -1;
    transition.rotation = 0;
    unsigned int start = get_time();
    map_leave_map(&transition);
    map_check_state();
    autotest_log("%s loaded in %u ms (screen %dx%d)", map_data.name, elapsed_time(start), screenGetWidth(), screenGetHeight());
    return 0;
}

// Frame packing comes back exactly: random, empty, flat, patterns, and
// the real screen of a map.
static int coop_autotest_pack(AutotestNewGameProc* newGame)
{
    std::vector<unsigned char> data;
    unsigned int seed = 12345;
    auto check = [&](const char* what) {
        int packed = coop_net_pack_selftest(data.data(), data.size());
        autotest_log("pack %s: %zu -> %d bytes", what, data.size(), packed);
        if (packed < 0) {
            autotest_fail("packing %s did not come back the same", what);
        }
    };

    data.clear();
    check("nothing");
    data.assign(1, 7);
    check("one byte");
    data.assign(5, 9);
    check("five bytes");
    data.assign(1000000, 0);
    check("a million zeros");
    data.resize(300000);
    for (unsigned char& byte : data) {
        seed = seed * 1103515245 + 12345;
        byte = (unsigned char)(seed >> 16);
    }
    check("random");
    for (size_t index = 0; index < data.size(); index++) {
        data[index] = (unsigned char)((index % 37) * 3 + (index / 5000));
    }
    check("pattern");
    for (int round = 0; round < 50; round++) {
        size_t size = 1 + (seed >> 8) % 70000;
        data.resize(size);
        for (unsigned char& byte : data) {
            seed = seed * 1103515245 + 12345;
            byte = (unsigned char)((seed >> 24) % 4);
        }
        if (coop_net_pack_selftest(data.data(), data.size()) < 0) {
            autotest_fail("packing a few-values buffer of %zu bytes did not come back the same", size);
        }
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }
    tile_refresh_display();
    data.assign((const unsigned char*)gSdlSurface->pixels, (const unsigned char*)gSdlSurface->pixels + (size_t)gSdlSurface->pitch * gSdlSurface->h);
    check("the screen");
    return 0;
}

// Single player (or with [coop]enabled=1 and player 2 added: co-op): on
// COOP_AUTOTEST_MAP, the critter named COOP_AUTOTEST_KILL dies as in a fight
// (its death script, player 1 as the killer), then the game runs; the
// story flags before and after, to compare the two.
static int coop_autotest_story_kill(AutotestNewGameProc* newGame)
{
    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }
    if (coop_is_enabled() && coop_add_player("premade\\stealth.gcd") == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    const char* mapName = getenv("COOP_AUTOTEST_MAP");
    const char* killName = getenv("COOP_AUTOTEST_KILL");
    MapTransition transition;
    transition.map = map_match_map_name(mapName != NULL ? mapName : "MSTRLR34.MAP");
    transition.elevation = 0;
    transition.tile = -1;
    transition.rotation = 0;
    map_leave_map(&transition);
    map_check_state();
    coop_autotest_log_story("before");

    Object* victim = NULL;
    for (Object* obj = obj_find_first(); obj != NULL && victim == NULL; obj = obj_find_next()) {
        if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && !critter_is_dead(obj) && strcmp(critter_name(obj), killName != NULL ? killName : "Master") == 0) {
            victim = obj;
        }
    }
    if (victim == NULL) {
        autotest_fail("nobody to kill");
        return -1;
    }
    // Players' turns in the fight that follows pass by themselves.
    coop_set_turn_hook(coop_autotest_pass_turn);
    autotest_log("killing %s (pid %x, script %d)", critter_name(victim), victim->pid, victim->sid);
    critter_kill(victim, -1, true);
    if (victim->sid != -1) {
        scr_set_objs(victim->sid, obj_dude, NULL);
        exec_script_proc(victim->sid, SCRIPT_PROC_DESTROY);
    }
    coop_autotest_frames_until(15000, NULL);
    coop_set_turn_hook(NULL);
    coop_autotest_log_story("after");
    autotest_log("players alive: %d, quit %d", critter_is_dead(obj_dude) ? 0 : 1, game_user_wants_to_quit);
    return 0;
}

// Runs the game for up to `ms`, until `done` (if given) says so.
static bool coop_autotest_frames_until(unsigned int ms, bool (*done)())
{
    unsigned int start = get_time();
    while (elapsed_time(start) < ms) {
        coop_autotest_host_frame();
        if (done != NULL && done()) {
            return true;
        }
    }
    return done == NULL;
}

static Object* coop_autotest_interact_item = NULL;
static Object* coop_autotest_interact_door = NULL;

static bool coop_autotest_item_picked_up()
{
    return coop_autotest_interact_item->owner == obj_dude;
}

static bool coop_autotest_door_open()
{
    return obj_is_open(coop_autotest_interact_door) != 0;
}

static bool coop_autotest_anims_done()
{
    return !anim_busy(obj_dude);
}

// ONE PC (F9) with player 2 in control, in Vault 13: player 2 picks up an
// item, opens a door and uses a stimpak. (Locks are up to door scripts:
// the engine itself neither keeps unscripted doors shut nor picks them.)
static int coop_autotest_interact(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p1 = coop_player(0);
    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    MapTransition transition;
    transition.map = map_match_map_name("VAULT13.MAP");
    transition.elevation = 0;
    transition.tile = -1;
    transition.rotation = 0;
    map_leave_map(&transition);
    map_check_state();
    coop_autotest_frames_until(1000, NULL);
    autotest_log("map %s, p1 tile %d, p2 tile %d", map_data.name, p1->obj->tile, p2->obj->tile);

    // COOP_AUTOTEST_INTERACT_P1: the same with player 1, for comparison.
    bool asPlayer1 = getenv("COOP_AUTOTEST_INTERACT_P1") != NULL;
    if (!asPlayer1) {
        coop_handle_switch_key();
        if (obj_dude != p2->obj) {
            autotest_fail("F9 did not give player 2 control");
            return -1;
        }
    }
    PlayerState* actor = asPlayer1 ? p1 : p2;

    // 1. An item on the ground next to player 2.
    Object* item;
    if (obj_pid_new(&item, PROTO_ID_STIMPACK) == -1) {
        autotest_fail("no stimpak");
        return -1;
    }
    obj_attempt_placement(item, obj_dude->tile, obj_dude->elevation, 1);
    coop_autotest_interact_item = item;
    action_get_an_object(obj_dude, item);
    if (!coop_autotest_frames_until(8000, coop_autotest_item_picked_up)) {
        autotest_fail("player 2 did not pick up the stimpak (owner %p)", (void*)item->owner);
    } else {
        autotest_log("player 2 picked up the stimpak");
    }
    coop_autotest_frames_until(5000, coop_autotest_anims_done);

    // 2. The nearest unscripted door: open it.
    Object* door = NULL;
    int best = 9999;
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        Proto* proto;
        if (PID_TYPE(obj->pid) != OBJ_TYPE_SCENERY || proto_ptr(obj->pid, &proto) == -1
            || proto->scenery.type != SCENERY_TYPE_DOOR || obj->sid != -1) {
            continue;
        }
        int distance = obj_dist(obj_dude, obj);
        if (distance < best && make_path(obj_dude, obj_dude->tile, obj->tile, NULL, 0) > 0) {
            best = distance;
            door = obj;
        }
    }
    if (door == NULL) {
        autotest_fail("no door to try");
        return -1;
    }
    coop_autotest_interact_door = door;
    autotest_log("door at tile %d, distance %d, open %d", door->tile, best, obj_is_open(door));
    if (obj_is_open(door)) {
        obj_close(door);
        coop_autotest_frames_until(2000, NULL);
    }
    action_use_an_object(obj_dude, door);
    if (!coop_autotest_frames_until(8000, coop_autotest_door_open)) {
        autotest_fail("player 2 did not open the door");
    } else {
        autotest_log("player 2 opened the door");
    }
    coop_autotest_frames_until(5000, coop_autotest_anims_done);

    // 3. The stimpak, used on themself while hurt.
    critter_adjust_hits(obj_dude, -10);
    int hurt = critter_get_hits(obj_dude);
    // As the inventory's USE does.
    if (item_d_take_drug(obj_dude, item) == 1) {
        item_remove_mult(obj_dude, item, 1);
        obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
        obj_destroy(item);
    }
    coop_autotest_frames_until(2000, NULL);
    autotest_log("stimpak: hp %d -> %d", hurt, critter_get_hits(obj_dude));
    if (critter_get_hits(obj_dude) <= hurt) {
        autotest_fail("the stimpak did not heal player 2");
    }

    if (obj_dude != actor->obj) {
        autotest_fail("control changed unexpectedly");
    }
    return 0;
}

// Sum of the radiation sickness bonuses on the primary stats.
static int coop_autotest_rad_penalty(Object* obj)
{
    int sum = 0;
    for (int stat = STAT_STRENGTH; stat <= STAT_AGILITY; stat++) {
        sum += stat_get_bonus(obj, stat);
    }
    return sum;
}

static void coop_autotest_pass_hours(int hours)
{
    for (int hour = 0; hour < hours; hour++) {
        inc_game_time(GAME_TIME_TICKS_PER_HOUR);
        queue_process();
    }
}

// Radiation sickness: player 2 with lots of rads gets sick (their stats
// drop) and gets better a week later; player 1 without rads stays well.
static int coop_autotest_rads(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p1 = coop_player(0);
    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    int p1Before = coop_autotest_rad_penalty(p1->obj);
    int p2Before = coop_autotest_rad_penalty(p2->obj);
    {
        ActivePlayerScope scope(p2);
        critter_adjust_rads(p2->obj, 450);
    }
    autotest_log("player 2 rads %d, player 1 rads %d", critter_get_rads(p2->obj), critter_get_rads(p1->obj));

    coop_autotest_pass_hours(30);
    int p1Sick = coop_autotest_rad_penalty(p1->obj);
    int p2Sick = coop_autotest_rad_penalty(p2->obj);
    autotest_log("after 30 hours: player 1 stat bonus %d -> %d, player 2 %d -> %d, player 2 hp %d",
        p1Before, p1Sick, p2Before, p2Sick, critter_get_hits(p2->obj));
    if (p2Sick >= p2Before) {
        autotest_fail("player 2 did not get radiation sickness");
    }
    if (p1Sick != p1Before) {
        autotest_fail("player 1 got player 2's radiation sickness");
    }
    if (critter_is_dead(p2->obj) || critter_is_dead(p1->obj)) {
        autotest_fail("a player died");
    }

    // The sickness wears off after a week (the rads stay until treated).
    {
        ActivePlayerScope scope(p2);
        critter_adjust_rads(p2->obj, -critter_get_rads(p2->obj));
    }
    coop_autotest_pass_hours(8 * 24);
    int p2Well = coop_autotest_rad_penalty(p2->obj);
    autotest_log("after 8 more days: player 2 stat bonus %d", p2Well);
    if (p2Well != p2Before) {
        autotest_fail("player 2's radiation sickness did not wear off");
    }

    // Both sick at once: each keeps their own sickness and recovery.
    for (int index = 0; index < 2; index++) {
        ActivePlayerScope scope(coop_player(index));
        critter_adjust_rads(obj_dude, 450);
    }
    coop_autotest_pass_hours(30);
    int p1Both = coop_autotest_rad_penalty(p1->obj);
    int p2Both = coop_autotest_rad_penalty(p2->obj);
    autotest_log("both sick: player 1 stat bonus %d, player 2 %d", p1Both, p2Both);
    if (p1Both >= p1Before || p2Both >= p2Before) {
        autotest_fail("both players should be sick");
    }
    for (int index = 0; index < 2; index++) {
        ActivePlayerScope scope(coop_player(index));
        critter_adjust_rads(obj_dude, -critter_get_rads(obj_dude));
    }
    coop_autotest_pass_hours(8 * 24);
    autotest_log("both well again: player 1 stat bonus %d, player 2 %d",
        coop_autotest_rad_penalty(p1->obj), coop_autotest_rad_penalty(p2->obj));
    if (coop_autotest_rad_penalty(p1->obj) != p1Before || coop_autotest_rad_penalty(p2->obj) != p2Before) {
        autotest_fail("radiation sickness did not wear off for both");
    }

    return 0;
}

static int coop_autotest_travel(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p1 = coop_player(0);
    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    // A scripted item in player 2's pack must keep its script across map
    // changes and saves, like the party's items do.
    Object* scriptedItem = NULL;
    for (int pid = 1; pid < 1000 && scriptedItem == NULL; pid++) {
        Proto* proto;
        if (proto_ptr(pid, &proto) == -1 || proto->sid == -1) {
            continue;
        }

        Object* item;
        if (obj_pid_new(&item, pid) == -1) {
            continue;
        }

        if (item->sid == -1 || item_add_force(p2->obj, item, 1) != 0) {
            obj_erase_object(item, NULL);
            continue;
        }

        obj_disconnect(item, NULL);
        scriptedItem = item;
        autotest_log("player 2 carries scripted item %s", proto_name(pid));
    }

    // Exit grids: find one leading to another map (not the world map,
    // whose screen would wait for input).
    Object* exitGrid = NULL;
    for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
        if (obj->pid >= 0x5000010 && obj->pid <= 0x5000017 && obj->data.misc.map > 0 && obj->elevation == obj_dude->elevation) {
            exitGrid = obj;
            break;
        }
    }

    char mapName[16];
    strcpy(mapName, map_data.name);

    if (exitGrid == NULL) {
        autotest_log("no exit grid to another map on %s; exit grid checks skipped", mapName);
    } else {
        autotest_log("exit grid at tile %d -> map %d", exitGrid->tile, exitGrid->data.misc.map);

        coop_set_controlled(p2);
        obj_move_to_tile(p2->obj, exitGrid->tile, exitGrid->elevation, NULL);
        coop_set_controlled(p1);
        map_check_state();
        if (strcmp(map_data.name, mapName) != 0) {
            autotest_fail("player 2 left the map through an exit grid");
            return -1;
        }
        autotest_log("player 2 on exit grid: stayed on %s", map_data.name);

        obj_move_to_tile(p1->obj, exitGrid->tile, exitGrid->elevation, NULL);
        map_check_state();
        if (strcmp(map_data.name, mapName) == 0) {
            autotest_fail("player 1 did not leave the map through an exit grid");
        } else {
            autotest_log("player 1 on exit grid: moved to %s", map_data.name);
        }

        coop_autotest_players_together("after exit grid");
    }

    // Script-style map change.
    MapTransition transition;
    transition.map = map_match_map_name("V13Ent.map");
    transition.elevation = 0;
    transition.tile = -1;
    transition.rotation = 0;
    map_leave_map(&transition);
    map_check_state();
    autotest_log("after load_map: %s", map_data.name);
    coop_autotest_players_together("after map change");

    // Elevation change: player 2 follows.
    int otherElevation = obj_dude->elevation == 0 ? 1 : 0;
    obj_move_to_tile(obj_dude, obj_dude->tile, otherElevation, NULL);
    map_set_elevation(otherElevation);
    coop_autotest_players_together("after elevation change");

    tile_refresh_display();
    dump_screen();

    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (SaveGame(LOAD_SAVE_MODE_QUICK) != 1) {
        autotest_fail("save after travel failed");
        return -1;
    }

    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (LoadGame(LOAD_SAVE_MODE_QUICK) != 1 || coop_player_count() != 2) {
        autotest_fail("load after travel failed");
        return -1;
    }

    coop_autotest_players_together("after load");

    if (scriptedItem != NULL) {
        int scripted = 0;
        Inventory* inventory = &(coop_player(1)->obj->data.inventory);
        for (int index = 0; index < inventory->length; index++) {
            Object* item = inventory->items[index].item;
            Script* script;
            if (item->sid != -1) {
                if (scr_ptr(item->sid, &script) == -1) {
                    autotest_fail("player 2's %s lost its script", object_name(item));
                } else {
                    scripted++;
                }
            }
        }
        autotest_log("player 2 has %d scripted items after travel and load", scripted);
        if (scripted == 0) {
            autotest_fail("player 2's scripted item is gone");
        }
    }

    return 0;
}

// Points of a 640x480 layout that the game centers on bigger screens
// (character editor, the view around the player) are queued with this
// flag and placed when used: a client only knows player 1's frame size
// once frames arrive, and its frames can be scaled to fit.
#define COOP_AUTOTEST_CENTERED 0x4000

static void coop_autotest_mouse_transform(int* x, int* y)
{
    bool client = coop_net_mode() == COOP_NET_CLIENT;
    // The layout is centered on the host's screen; a client's frames may
    // be that screen scaled down.
    int hostWidth = screenGetWidth();
    int hostHeight = screenGetHeight();
    int frameWidth = 0;
    int frameHeight = 0;
    bool frames = client && coop_net_client_frame_size(&frameWidth, &frameHeight);
    if (client && !coop_net_client_host_size(&hostWidth, &hostHeight) && frames) {
        hostWidth = frameWidth;
        hostHeight = frameHeight;
    }
    if ((*x & COOP_AUTOTEST_CENTERED) != 0) {
        *x = (*x & ~COOP_AUTOTEST_CENTERED) + (hostWidth - 640) / 2;
        *y += (hostHeight - 480) / 2;
    }
    if (frames) {
        // Host screen -> frame (centre of the pixel) -> this window.
        *x = (2 * *x + 1) * frameWidth / (2 * hostWidth);
        *y = (2 * *y + 1) * frameHeight / (2 * hostHeight);
        coop_net_client_frame_to_window(x, y);
    }
}

static void coop_autotest_click_centered(int x, int y)
{
    inject_set_mouse_transform(coop_autotest_mouse_transform);
    inject_click(x | COOP_AUTOTEST_CENTERED, y);
}

// The F9 debug key: player 2 joins through the character creation screen
// (driven by injected key presses), then control switches back and forth.
static int coop_autotest_join(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    Object* p1obj = obj_dude;

    // Cancelling character creation leaves the game as it was.
    inject_key(KEY_ESCAPE);
    coop_handle_switch_key();
    if (coop_player_count() != 1 || obj_dude != p1obj) {
        autotest_fail("cancelled creation still added player 2");
        return -1;
    }
    autotest_log("creation cancelled: still one player");

    // Character creation by mouse (640x480 editor layout): tag three
    // skills, spend the 5 character points on Strength, screenshot, Done.
    inject_wait(10);
    coop_autotest_click_centered(355, 31);
    inject_wait(20);
    coop_autotest_click_centered(355, 42);
    inject_wait(20);
    coop_autotest_click_centered(355, 53);
    inject_wait(20);
    // The stat slider runs its own repeat loop; give it time to settle.
    for (int index = 0; index < 5; index++) {
        coop_autotest_click_centered(157, 42);
        inject_wait(20);
    }
    inject_key(KEY_F12);
    inject_wait(3);
    // Done, then pick the second look (leather jacket).
    inject_key(KEY_RETURN);
    inject_wait(30);
    inject_key(KEY_ARROW_DOWN);
    inject_wait(3);
    inject_key(KEY_RETURN);
    coop_handle_switch_key();

    // Drop whatever the editor did not consume.
    while (inject_pending()) {
        inject_update();
    }

    if (coop_player_count() != 2) {
        autotest_fail("player 2 was not created");
        return -1;
    }

    PlayerState* p2 = coop_player(1);
    autotest_log("player 2 '%s' joined: level %d, ST %d, %d/%d hp", p2->name,
        p2->pcStats[PC_STAT_LEVEL], stat_level(p2->obj, STAT_STRENGTH), critter_get_hits(p2->obj), stat_level(p2->obj, STAT_MAXIMUM_HIT_POINTS));

    {
        ActivePlayerScope scope(p2);
        autotest_dump_state("p2_created.txt");
    }

    if (critter_get_hits(p2->obj) != stat_level(p2->obj, STAT_MAXIMUM_HIT_POINTS)) {
        autotest_fail("player 2 does not start at full health");
    }

    int jacket = art_critter_index(stat_level(p2->obj, STAT_GENDER) == GENDER_MALE ? "hmmaxx" : "hfmaxx");
    autotest_log("player 2 look %d, art %d (leather jacket art %d)", p2->look, p2->obj->fid & 0xFFF, jacket);
    if (p2->look != COOP_LOOK_LEATHER_JACKET || (p2->obj->fid & 0xFFF) != jacket) {
        autotest_fail("player 2 does not wear the chosen look");
    }

    tile_refresh_display();
    dump_screen();

    coop_handle_switch_key();
    if (obj_dude != p2->obj) autotest_fail("F9 did not switch to player 2");

    tile_refresh_display();
    dump_screen();

    coop_handle_switch_key();
    if (obj_dude != p1obj) autotest_fail("F9 did not switch back to player 1");

    return 0;
}

// Not a test: lists the critter models that have every animation a player
// character needs (so none falls back to another model), and shows them
// standing in a row next to player 1, as candidates for player 2's look.
static int coop_autotest_looks(AutotestNewGameProc* newGame)
{
    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    // Unarmed and every weapon type (WEAPON_ANIMATION_KNIFE .. LAUNCHER).
    static const int baseAnims[] = { ANIM_STAND, ANIM_WALK, ANIM_RUNNING, ANIM_THROW_PUNCH, ANIM_KICK_LEG, ANIM_DODGE_ANIM, ANIM_HIT_FROM_FRONT, ANIM_FALL_BACK };

    int candidates[32];
    int candidatesCount = 0;

    for (int index = 0; index < 1000; index++) {
        int standFid = art_id(OBJ_TYPE_CRITTER, index, ANIM_STAND, 0, 0);
        char* path = art_get_name(standFid);
        if (path == NULL || !art_exists(standFid)) {
            continue;
        }

        char name[16];
        const char* fileName = strrchr(path, '\\');
        strncpy(name, fileName != NULL ? fileName + 1 : path, 6);
        name[6] = '\0';

        int present = 0;
        int total = 0;
        for (int anim : baseAnims) {
            total++;
            present += art_exists((OBJ_TYPE_CRITTER << 24) | (anim << 16) | index) ? 1 : 0;
        }

        for (int weapon = WEAPON_ANIMATION_KNIFE; weapon <= WEAPON_ANIMATION_LAUNCHER; weapon++) {
            total++;
            present += art_exists((OBJ_TYPE_CRITTER << 24) | (ANIM_TAKE_OUT << 16) | (weapon << 12) | index) ? 1 : 0;
        }

        if (present * 100 / total >= 80) {
            autotest_log("look %d: %s %d/%d animations", index, name, present, total);
            // Even the vault suit lacks one, so allow that.
            if (present >= total - 1 && candidatesCount < 32) {
                candidates[candidatesCount++] = index;
            }
        }
    }

    // Show the complete ones in rows of 6.
    int shown = 0;
    while (shown < candidatesCount) {
        Object* row[6];
        int rowCount = 0;
        int tile = tile_num_in_direction(obj_dude->tile, 1, 2);
        for (; rowCount < 6 && shown < candidatesCount; rowCount++, shown++) {
            Object* obj;
            obj_new(&obj, art_id(OBJ_TYPE_CRITTER, candidates[shown], ANIM_STAND, 0, 2), 0x1000001);
            obj_move_to_tile(obj, tile, obj_dude->elevation, NULL);
            obj_set_rotation(obj, 2, NULL);
            obj_set_light(obj, 2, 0x10000, NULL);
            row[rowCount] = obj;
            autotest_log("screenshot %d position %d: look %d", shown / 6, rowCount + 1, candidates[shown]);
            tile = tile_num_in_direction(tile, 2, 2);
        }

        tile_set_center(row[rowCount / 2]->tile, TILE_SET_CENTER_REFRESH_WINDOW);
        tile_refresh_display();
        dump_screen();

        for (int index = 0; index < rowCount; index++) {
            obj_erase_object(row[index], NULL);
        }
    }

    return 0;
}

// Player 2 reaching a merchant trades directly (no conversation).
static int coop_autotest_barter(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    coop_autotest_scene();

    // A merchant of the map by name (COOP_AUTOTEST_MERCHANT), else a
    // generic one.
    Object* localMerchant = NULL;
    const char* merchantName = getenv("COOP_AUTOTEST_MERCHANT");
    if (merchantName != NULL) {
        for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
            if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && strstr(critter_name(obj), merchantName) != NULL) {
                localMerchant = obj;
                break;
            }
        }
        if (localMerchant == NULL) {
            autotest_log("no %s on this map", merchantName);
        }
    }

    int merchantPid = -1;
    for (int index = 1; index < 400 && merchantPid == -1; index++) {
        Proto* proto;
        if (proto_ptr(0x1000000 | index, &proto) != -1 && compat_stricmp(proto_name(0x1000000 | index), "Merchant") == 0) {
            merchantPid = 0x1000000 | index;
        }
    }

    if (getenv("COOP_AUTOTEST_HEADS") != NULL) {
        for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
            Proto* proto;
            if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && proto_ptr(obj->pid, &proto) != -1) {
                autotest_log("head %s pid %x headFid %x barter %d", critter_name(obj), obj->pid, proto->critter.headFid, (proto->critter.data.flags & CRITTER_BARTER) != 0);
            }
        }
    }

    Object* merchant = localMerchant;
    if (merchant != NULL) {
        obj_attempt_placement(p2->obj, merchant->tile, merchant->elevation, 1);
        tile_set_center(p2->obj->tile, TILE_SET_CENTER_REFRESH_WINDOW);
    } else if (merchantPid == -1 || obj_pid_new(&merchant, merchantPid) == -1) {
        autotest_fail("could not create a merchant");
        return -1;
    } else {
        obj_attempt_placement(merchant, p2->obj->tile, p2->obj->elevation, 1);
    }

    Object* stimpak;
    if (obj_pid_new(&stimpak, PROTO_ID_STIMPACK) == 0 && item_add_force(merchant, stimpak, 2) == 0) {
        obj_disconnect(stimpak, NULL);
    }

    if (!coop_can_barter_with(merchant)) {
        autotest_fail("merchant does not barter");
        return -1;
    }

    if (coop_can_barter_with(obj_dude)) {
        autotest_fail("player 1 counts as a merchant");
    }

    // Screenshot of the barter screen, then leave it.
    inject_wait(20);
    inject_call(dump_screen);
    inject_wait(3);
    inject_key(KEY_ESCAPE);

    coop_request_barter(p2, merchant);
    coop_process_requests();

    while (inject_pending()) {
        inject_update();
    }

    if (obj_dude != coop_player(0)->obj || coop_active_player() != coop_player(0)) {
        autotest_fail("barter left player 2 active");
    }

    autotest_log("barter with %s done", critter_name(merchant));

    tile_refresh_display();
    dump_screen();

    return 0;
}

// COOP_AUTOTEST_RELAY=host:port: the host opens a relay room and writes
// its code to COOP_AUTOTEST_RELAY_FILE; the client joins that code.
static bool coop_autotest_relay_host_setup()
{
    const char* relay = getenv("COOP_AUTOTEST_RELAY");
    const char* file = getenv("COOP_AUTOTEST_RELAY_FILE");
    if (relay == NULL || file == NULL) {
        return true;
    }

    coop_net_set_relay_server(relay);
    std::string error;
    const char* wanted = getenv("COOP_AUTOTEST_RELAY_CODE");
    if (!coop_net_relay_host_start(&error, wanted != NULL ? wanted : "")) {
        autotest_fail("relay: %s", error.c_str());
        return false;
    }

    FILE* stream = fopen(file, "w");
    if (stream != NULL) {
        fprintf(stream, "%s\n", coop_net_relay_code());
        fclose(stream);
    }
    autotest_log("relay code %s", coop_net_relay_code());
    return true;
}

static void coop_autotest_relay_client_setup()
{
    const char* relay = getenv("COOP_AUTOTEST_RELAY");
    const char* file = getenv("COOP_AUTOTEST_RELAY_FILE");
    if (relay == NULL || file == NULL) {
        return;
    }

    char code[32] = "";
    unsigned int start = get_time();
    while (elapsed_time(start) < 30000) {
        FILE* stream = fopen(file, "r");
        if (stream != NULL) {
            bool read = fgets(code, sizeof(code), stream) != NULL;
            fclose(stream);
            if (read && strlen(code) >= 6) {
                break;
            }
        }
        SDL_Delay(100);
    }
    code[strcspn(code, "\r\n")] = '\0';

    // Typed by hand, with a dash, as a player might.
    std::string typed = std::string(code).substr(0, 3) + "-" + std::string(code).substr(3);
    std::string parsed;
    if (!coop_net_parse_code(typed.c_str(), &parsed)) {
        autotest_fail("relay code '%s' not understood", typed.c_str());
    }
    coop_net_set_relay_server(relay);
    coop_net_set_relay_join(parsed);
    autotest_log("joining relay code %s", parsed.c_str());
}

// Network: host a game and wait (up to 30 s) for a client to join, then
// for it to leave. Paired with "net_client" by tools/nettest.sh.
static int coop_autotest_net_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    // Found by LAN searches (the server browser), like a real host; with
    // COOP_AUTOTEST_LOBBY_SERVER also listed online there.
    const char* lobbyServer = getenv("COOP_AUTOTEST_LOBBY_SERVER");

    if (!coop_net_host_start()) {
        autotest_fail("cannot host");
        return -1;
    }

    // With COOP_AUTOTEST_RELAY the listing carries the relay code.
    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }
    coop_lobby_host_start(lobbyServer != NULL ? lobbyServer : "", lobbyServer != NULL);

    autotest_log("hosting, checksum %016llx", (unsigned long long)coop_net_data_checksum());

    unsigned int start = get_time();
    while (elapsed_time(start) < 30000 && !coop_net_client_connected()) {
        coop_net_host_pump();
        coop_lobby_host_pump(critter_name(obj_dude), coop_net_client_connected() ? 2 : 1);
        SDL_Delay(10);
    }

    if (!coop_net_client_connected()) {
        autotest_fail("no client joined");
        return -1;
    }

    autotest_log("client joined");

    // Stream a few seconds of frames (each present sends one), with a
    // status line, then take a screenshot and leave; the client takes its
    // own when we are gone, and the two must show the same picture.
    coop_net_send_status("Player 1 is testing");

    // Speech reaches the client too (the hook is set by coop_host_init();
    // here it is set by hand).
    gsound_event_hook = coop_net_send_sound;
    gsound_speech_play("narrator\\options", 12, 13, 15);

    start = get_time();
    while (elapsed_time(start) < 3000) {
        tile_refresh_display();
        renderPresent();
        SDL_Delay(30);
    }

    dump_screen();
    coop_lobby_host_stop();
    coop_net_exit();

    autotest_log("left");
    return 0;
}

static void coop_autotest_net_client_exit()
{
    dump_screen();
}

static int coop_autotest_net_client(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_CLIENT) {
        autotest_fail("needs [coop]mode=client");
        return -1;
    }

    coop_autotest_relay_client_setup();
    autotest_log("connecting, checksum %016llx", (unsigned long long)coop_net_data_checksum());
    coop_net_set_client_exit_hook(coop_autotest_net_client_exit);
    int rc = coop_net_client_run();
    autotest_log("client finished with %d", rc);
    if (rc != 0) {
        autotest_fail("client failed");
    }

    return 0;
}

// Network play, host side: runs the real main loop pieces while the client
// creates player 2 and walks them somewhere. Paired with net_play_client.
static int coop_autotest_net_play_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    coop_host_init();
    if (!coop_host_active()) {
        autotest_fail("cannot host");
        return -1;
    }

    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }

    int startTile = -1;
    bool moved = false;
    const int chatKeys[] = { KEY_LOWERCASE_T, 'W', 'a', 'r', ' ', 'n', 'e', 'v', 'e', 'r', ' ', 'c', 'h', 'a', 'n', 'g', 'e', 's', KEY_RETURN };
    int chatKey = 0;
    unsigned int start = get_time();
    while (elapsed_time(start) < 90000) {
        sharedFpsLimiter.mark();

        coop_host_begin_main_input();
        int keyCode = get_input();
        coop_host_end_main_input();
        game_handle_input(keyCode, false);

        // Player 1 chats once player 2 is there: T, a message, Enter (one
        // key a frame, through player 1's own input handling).
        if (startTile != -1 && chatKey < (int)(sizeof(chatKeys) / sizeof(chatKeys[0]))) {
            game_handle_input(chatKeys[chatKey++], false);
        }

        scripts_check_state();
        coop_process_requests();
        map_check_state();
        coop_host_frame();
        renderPresent();

        sharedFpsLimiter.throttle();

        if (coop_player_count() > 1) {
            Object* p2 = coop_player(1)->obj;
            if (startTile == -1) {
                startTile = p2->tile;
                autotest_log("player 2 '%s' joined at tile %d, look %d", coop_player(1)->name, startTile, coop_player(1)->look);
            } else if (p2->tile != startTile && !moved) {
                moved = true;
                autotest_log("player 2 moving (tile %d)", p2->tile);
            }
        }

        if (!coop_net_client_connected() && startTile != -1) {
            break;
        }
    }

    if (startTile == -1) {
        autotest_fail("player 2 never joined");
    } else if (!moved) {
        autotest_fail("player 2 never moved");
    } else {
        autotest_log("player 2 ended at tile %d (distance %d from start)", coop_player(1)->obj->tile,
            tile_dist(startTile, coop_player(1)->obj->tile));
    }

    dump_screen();
    coop_host_exit();
    return 0;
}

static void coop_autotest_screenshot()
{
    dump_screen();
}

// Network play, client side: drives character creation through the host,
// then clicks the map to walk player 2.
static int coop_autotest_net_play_client(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_CLIENT) {
        autotest_fail("needs [coop]mode=client");
        return -1;
    }

    coop_autotest_relay_client_setup();

    // Character creation (same layout as coop_join): tag three skills,
    // Strength +5, Done, then the look list: leather jacket.
    inject_wait(240);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_centered(355, 31);
    inject_wait(20);
    coop_autotest_click_centered(355, 42);
    inject_wait(20);
    coop_autotest_click_centered(355, 53);
    inject_wait(20);
    for (int index = 0; index < 5; index++) {
        coop_autotest_click_centered(157, 42);
        inject_wait(20);
    }
    inject_key(KEY_RETURN);
    inject_wait(120);
    inject_call(coop_autotest_screenshot); // the look list
    inject_key(KEY_ARROW_DOWN);
    inject_wait(10);
    inject_key(KEY_RETURN);

    // Free roam in player 2's own view: walk to a spot right of center.
    inject_wait(180);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_centered(420, 250);
    inject_wait(240);
    inject_call(coop_autotest_screenshot);

    // Chat: T, a message (the I must not open the inventory), Enter.
    inject_key(KEY_LOWERCASE_T);
    inject_wait(5);
    const char* chat = "Hi from the Vault!";
    for (const char* c = chat; *c != '\0'; c++) {
        inject_key(*c);
        inject_wait(2);
    }
    inject_wait(30);
    inject_call(coop_autotest_screenshot); // typing
    inject_key(KEY_RETURN);
    inject_wait(30);
    inject_call(coop_autotest_screenshot); // sent

    // Player 2's inventory: open, look, close.
    inject_key(KEY_LOWERCASE_I);
    inject_wait(120);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_ESCAPE);
    inject_wait(120);
    inject_call(coop_net_client_request_exit);

    coop_net_set_client_exit_hook(coop_autotest_net_client_exit);
    int rc = coop_net_client_run();
    autotest_log("client finished with %d", rc);
    if (rc != 0) {
        autotest_fail("client failed");
    }

    return 0;
}

// Network combat, host side: player 2 already exists (as from a save), the
// client joins, then player 1 (hooked) and the enemy fight while player 2's
// turns are ended by the client's Space presses over the network.
static int coop_autotest_net_combat_player2_turns = 0;

static bool coop_autotest_net_combat_turn(Object* player)
{
    if (player == coop_player(0)->obj) {
        // COOP_AUTOTEST_COMBAT_CHAT: player 1 chats in their first turn
        // (the space must not end it), then ends it with Space.
        static bool chatted = false;
        if (getenv("COOP_AUTOTEST_COMBAT_CHAT") != NULL && !chatted) {
            chatted = true;
            inject_key(KEY_LOWERCASE_T);
            for (const char* c = "on my way"; *c != '\0'; c++) {
                inject_key(*c);
            }
            inject_key(KEY_RETURN);
            inject_wait(5);
            inject_key(KEY_SPACE);
            return false;
        }
        return coop_autotest_attack_turn(player);
    }

    // Player 2: played by the client.
    coop_autotest_net_combat_player2_turns++;
    autotest_log("player 2's turn %d", coop_autotest_net_combat_player2_turns);
    return false;
}

static int coop_autotest_net_combat_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    coop_host_init();
    if (!coop_host_active()) {
        autotest_fail("cannot host");
        return -1;
    }
    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }

    unsigned int start = get_time();
    while (elapsed_time(start) < 30000 && !coop_net_client_connected()) {
        coop_net_host_pump();
        renderPresent();
        SDL_Delay(10);
    }

    if (!coop_net_client_connected()) {
        autotest_fail("no client joined");
        return -1;
    }

    // A few free roam frames: player 2 resumes without creating a character.
    for (int frame = 0; frame < 30; frame++) {
        coop_host_frame();
        renderPresent();
        SDL_Delay(15);
    }

    if (coop_player_count() != 2 || coop_player(1) != p2) {
        autotest_fail("player 2 was not kept for the joining client");
    }

    coop_autotest_scene();

    Object* enemy = coop_autotest_spawn_enemy(coop_player(0)->obj);
    if (enemy == NULL) {
        return -1;
    }

    coop_autotest_enemy = enemy;
    coop_autotest_turns_total = 0;
    memset(coop_autotest_turns, 0, sizeof(coop_autotest_turns));
    coop_set_turn_hook(coop_autotest_net_combat_turn);

    STRUCT_664980 attack;
    memset(&attack, 0, sizeof(attack));
    attack.attacker = coop_player(0)->obj;
    attack.defender = enemy;
    combat(&attack);

    coop_set_turn_hook(NULL);

    autotest_log("combat over: enemy dead %d, player 2 turns %d, players dead %d",
        critter_is_dead(enemy) ? 1 : 0, coop_autotest_net_combat_player2_turns, coop_any_player_has(DAM_DEAD) ? 1 : 0);

    if (coop_autotest_net_combat_player2_turns == 0) {
        autotest_fail("player 2 never had a turn");
    }

    if (!critter_is_dead(enemy) && !coop_any_player_has(DAM_DEAD)) {
        autotest_fail("combat ended without a result");
    }

    coop_host_exit();
    return 0;
}

static void coop_autotest_screenshot_forever()
{
    inject_wait(130);
    inject_call(coop_autotest_screenshot);
    inject_call(coop_autotest_screenshot_forever);
}

static void coop_autotest_press_space_forever()
{
    // Re-queues itself: Space every second until the host leaves.
    inject_wait(60);
    inject_key(KEY_SPACE);
    inject_call(coop_autotest_press_space_forever);
}

static int coop_autotest_net_combat_client(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_CLIENT) {
        autotest_fail("needs [coop]mode=client");
        return -1;
    }

    coop_autotest_relay_client_setup();

    inject_wait(400);
    inject_call(coop_autotest_screenshot);
    // COOP_AUTOTEST_COMBAT_CHAT: chat in a turn first; the space in the
    // message must not end the turn.
    if (getenv("COOP_AUTOTEST_COMBAT_CHAT") != NULL) {
        inject_key(KEY_LOWERCASE_T);
        for (const char* c = "hello there"; *c != '\0'; c++) {
            inject_key(*c);
            inject_wait(2);
        }
        inject_wait(10);
        inject_call(coop_autotest_screenshot);
        inject_key(KEY_RETURN);
        inject_wait(30);
    }
    inject_call(coop_autotest_press_space_forever);
    if (getenv("COOP_AUTOTEST_MAP") != NULL) {
        inject_call(coop_autotest_screenshot_forever);
    }

    int rc = coop_net_client_run();
    autotest_log("client finished with %d", rc);
    if (rc != 0) {
        autotest_fail("client failed");
    }

    return 0;
}

// Human combat turns played by a test hook.

static int coop_autotest_combat(AutotestNewGameProc* newGame)
{
    if (!coop_is_enabled()) {
        autotest_fail("needs [coop]enabled=1");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p1 = coop_player(0);
    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    // Fight 1: both players punch a weak enemy until it dies.
    Object* enemy = coop_autotest_spawn_enemy(p2->obj);
    if (enemy == NULL) {
        return -1;
    }

    int killType = critter_kill_count_type(enemy);
    int killXp = critter_kill_exps(enemy);
    int p1Kills;
    int p2Kills;
    int p1Xp;
    int p2Xp;
    {
        ActivePlayerScope scope(p1);
        p1Kills = critter_kill_count(killType);
        p1Xp = stat_pc_get(PC_STAT_EXPERIENCE);
    }
    {
        ActivePlayerScope scope(p2);
        p2Kills = critter_kill_count(killType);
        p2Xp = stat_pc_get(PC_STAT_EXPERIENCE);
    }

    coop_autotest_enemy = enemy;
    memset(coop_autotest_turns, 0, sizeof(coop_autotest_turns));
    coop_autotest_turns_total = 0;
    coop_set_turn_hook(coop_autotest_attack_turn);

    STRUCT_664980 attack;
    memset(&attack, 0, sizeof(attack));
    attack.attacker = p2->obj;
    attack.defender = enemy;
    combat(&attack);

    coop_set_turn_hook(NULL);

    autotest_log("fight 1: turns p1 %d, p2 %d; enemy dead %d",
        coop_autotest_turns[0], coop_autotest_turns[1], critter_is_dead(enemy) ? 1 : 0);

    if (isInCombat()) autotest_fail("fight 1 did not end");
    if (!critter_is_dead(enemy)) autotest_fail("enemy survived fight 1");
    if (coop_autotest_turns[1] == 0) autotest_fail("player 2 never got a human turn");
    if (coop_any_player_has(DAM_DEAD)) autotest_fail("a player died in fight 1");

    // Exactly one player gets the kill; both get the XP.
    {
        ActivePlayerScope scope(p1);
        int p1KillsAfter = critter_kill_count(killType);
        int p1XpAfter = stat_pc_get(PC_STAT_EXPERIENCE);
        ActivePlayerScope scope2(p2);
        int p2KillsAfter = critter_kill_count(killType);
        int p2XpAfter = stat_pc_get(PC_STAT_EXPERIENCE);

        autotest_log("kills p1 %d->%d, p2 %d->%d; xp p1 %d->%d, p2 %d->%d (kill xp %d)",
            p1Kills, p1KillsAfter, p2Kills, p2KillsAfter, p1Xp, p1XpAfter, p2Xp, p2XpAfter, killXp);

        if ((p1KillsAfter - p1Kills) + (p2KillsAfter - p2Kills) != 1) autotest_fail("kill not credited to exactly one player");
        if (p1XpAfter < p1Xp + killXp || p2XpAfter < p2Xp + killXp) autotest_fail("kill XP not given to both players");
    }

    tile_refresh_display();
    dump_screen();

    // Fight 2: players pass (by running out of [coop] turn_time when set,
    // else via the hook); the enemy kills a 1 HP player 2 -> game over.
    enemy = coop_autotest_spawn_enemy(p2->obj);
    if (enemy == NULL) {
        return -1;
    }

    critter_adjust_hits(p2->obj, 1 - critter_get_hits(p2->obj));

    coop_autotest_enemy = enemy;
    coop_autotest_turns_total = 0;
    if (coop_turn_time_limit() == 0) {
        coop_set_turn_hook(coop_autotest_pass_turn);
    }

    unsigned int fightStart = get_time();
    memset(&attack, 0, sizeof(attack));
    attack.attacker = enemy;
    attack.defender = p2->obj;
    combat(&attack);

    coop_set_turn_hook(NULL);

    autotest_log("fight 2: %d hooked turns, %u ms; p2 dead %d", coop_autotest_turns_total, elapsed_time(fightStart), critter_is_dead(p2->obj) ? 1 : 0);

    if (isInCombat()) autotest_fail("fight 2 did not end");
    if (!critter_is_dead(p2->obj)) autotest_fail("player 2 survived fight 2");
    if (!coop_any_player_has(DAM_DEAD)) autotest_fail("game over not detected");

    tile_refresh_display();
    dump_screen();

    return 0;
}

static void coop_autotest_count_turn(Object* player)
{
    PlayerState* state = coop_player_of(player);
    for (int index = 0; index < coop_player_count(); index++) {
        if (coop_player(index) == state) {
            coop_autotest_turns[index]++;
        }
    }

    // Safety net against fights that never end.
    if (++coop_autotest_turns_total > 200) {
        autotest_fail("combat did not finish in 200 player turns");
        combat_end();
    }
}

static bool coop_autotest_attack_turn(Object* player)
{
    coop_autotest_count_turn(player);

    Object* enemy = coop_autotest_enemy;
    if (enemy != NULL && !critter_is_dead(enemy) && obj_dist(player, enemy) <= 1) {
        autotest_log("%s punches %s", critter_name(player), critter_name(enemy));
        combat_attack(player, enemy, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
    }

    // Enemy gone: END COMBAT, as a player would (bystanders that joined
    // keep a fight going otherwise).
    if (enemy != NULL && critter_is_dead(enemy)) {
        combat_end();
    }

    return true;
}

static bool coop_autotest_pass_turn(Object* player)
{
    coop_autotest_count_turn(player);
    return true;
}

// Spawns a "Cave Rat" next to `nextTo`, with few hit
// points.
static Object* coop_autotest_spawn_enemy(Object* nextTo)
{
    int pid = -1;
    for (int index = 1; index < 400 && pid == -1; index++) {
        Proto* proto;
        if (proto_ptr(0x1000000 | index, &proto) != -1) {
            const char* enemyName = getenv("COOP_AUTOTEST_ENEMY");
            if (compat_stricmp(proto_name(0x1000000 | index), enemyName != NULL ? enemyName : "Cave Rat") == 0) {
                pid = 0x1000000 | index;
            }
        }
    }

    if (pid == -1) {
        autotest_fail("no Cave Rat proto found");
        return NULL;
    }

    Object* enemy;
    if (obj_pid_new(&enemy, pid) == -1) {
        autotest_fail("could not create enemy");
        return NULL;
    }

    obj_attempt_placement(enemy, nextTo->tile, nextTo->elevation, 1);
    const char* enemyHits = getenv("COOP_AUTOTEST_ENEMY_HP");
    int hits = enemyHits != NULL ? atoi(enemyHits) : 3;
    critter_adjust_hits(enemy, hits - critter_get_hits(enemy));

    autotest_log("spawned %s (pid %x) at tile %d, %d hp, team %d", critter_name(enemy), pid, enemy->tile,
        critter_get_hits(enemy), enemy->data.critter.combat.team);

    return enemy;
}

// For screenshots: with COOP_AUTOTEST_MAP set, both players (equipped)
// move to that map first.
static void coop_autotest_scene()
{
    const char* mapName = getenv("COOP_AUTOTEST_MAP");
    if (mapName == NULL || coop_player_count() < 2) {
        return;
    }

    const char* armor1[] = { "Metal Armor", "Leather Armor" };
    const char* weapon1[] = { "Hunting Rifle", "10mm Pistol" };
    const char* armor2[] = { "Leather Jacket", "Leather Armor" };
    const char* weapon2[] = { "10mm SMG", "10mm Pistol" };
    coop_autotest_equip(coop_player(0), armor1, 2, 0);
    coop_autotest_equip(coop_player(0), weapon1, 2, HAND_RIGHT);
    coop_autotest_equip(coop_player(1), armor2, 2, 0);
    coop_autotest_equip(coop_player(1), weapon2, 2, HAND_RIGHT);

    register_screendump(KEY_F12, NULL);

    MapTransition transition;
    transition.map = map_match_map_name(mapName);
    transition.elevation = 0;
    transition.tile = -1;
    transition.rotation = 0;
    if (transition.map == -1) {
        autotest_log("no map %s", mapName);
        return;
    }

    map_leave_map(&transition);
    map_check_state();
    autotest_log("scene: %s", map_data.name);
}

static bool coop_autotest_players_together(const char* when)
{
    Object* p1obj = coop_player(0)->obj;
    Object* p2obj = coop_player(1)->obj;

    autotest_log("%s: p1 tile %d elev %d, p2 tile %d elev %d, map elev %d",
        when, p1obj->tile, p1obj->elevation, p2obj->tile, p2obj->elevation, map_elevation);

    if (p2obj->elevation != p1obj->elevation || p1obj->elevation != map_elevation || p2obj->tile == -1 || obj_dist(p1obj, p2obj) > 5) {
        autotest_fail("%s: players are not together", when);
        return false;
    }

    return true;
}

// Stats and skills of `player` must be the same whether looked up from
// another player's scope or from its own.
static void coop_autotest_compare_lookups(PlayerState* player, const char* when)
{
    Object* obj = player->obj;
    int outside[STAT_COUNT + SKILL_COUNT];

    for (int stat = 0; stat < STAT_COUNT; stat++) {
        outside[stat] = stat_level(obj, stat);
    }

    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        outside[STAT_COUNT + skill] = skill_level(obj, skill);
    }

    ActivePlayerScope scope(player);

    for (int stat = 0; stat < STAT_COUNT; stat++) {
        if (stat_level(obj, stat) != outside[stat]) {
            autotest_fail("%s: stat %d of %s is %d from outside, %d inside", when, stat, critter_name(obj), outside[stat], stat_level(obj, stat));
        }
    }

    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        if (skill_level(obj, skill) != outside[STAT_COUNT + skill]) {
            autotest_fail("%s: skill %d of %s is %d from outside, %d inside", when, skill, critter_name(obj), outside[STAT_COUNT + skill], skill_level(obj, skill));
        }
    }
}

// Compares two text files line by line, ignoring lines starting with
// `skipPrefix`.
static bool coop_autotest_files_equal_except(const char* path1, const char* path2, const char* skipPrefix)
{
    FILE* stream1 = fopen(path1, "rt");
    FILE* stream2 = fopen(path2, "rt");
    bool equal = stream1 != NULL && stream2 != NULL;
    size_t prefixLength = strlen(skipPrefix);

    char line1[256];
    char line2[256];
    while (equal) {
        char* read1;
        do {
            read1 = fgets(line1, sizeof(line1), stream1);
        } while (read1 != NULL && strncmp(line1, skipPrefix, prefixLength) == 0);

        char* read2;
        do {
            read2 = fgets(line2, sizeof(line2), stream2);
        } while (read2 != NULL && strncmp(line2, skipPrefix, prefixLength) == 0);

        if (read1 == NULL || read2 == NULL) {
            equal = read1 == read2;
            break;
        }

        equal = strcmp(line1, line2) == 0;
    }

    if (stream1 != NULL) {
        fclose(stream1);
    }

    if (stream2 != NULL) {
        fclose(stream2);
    }

    return equal;
}

// Screenshot of the main menu.
static int coop_autotest_main_menu(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);
    renderPresent();
    dump_screen();
    main_menu_hide(false);
    return 0;
}

// Main menu button `slot` (0-5), as the mouse clicks it.
static void coop_autotest_click_menu_slot(int slot)
{
    // The main menu is centered on bigger screens.
    coop_autotest_click_centered(438, slot * 41 + 58);
    inject_wait(15);
}

// The multiplayer menu, driven like a player would: main menu MULTIPLAYER,
// JOIN GAME (type an address, give up), then HOST GAME and NEW GAME.
static int coop_autotest_menu_flow(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    inject_wait(10);
    coop_autotest_click_menu_slot(3);
    int rc = main_menu_loop();
    if (rc != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen (%d)", rc);
        return -1;
    }

    inject_wait(10);
    inject_call(coop_autotest_screenshot);
    // JOIN GAME, type an address, screenshot, Escape.
    coop_autotest_click_menu_slot(1);
    inject_wait(10);
    const char* address = "192.168.1.10";
    for (const char* p = address; *p != '\0'; p++) {
        inject_key(*p);
        inject_wait(2);
    }
    inject_wait(10);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_ESCAPE);
    inject_wait(15);
    // HOST GAME, screenshot, NEW GAME, screenshot of the hosting message,
    // close it.
    coop_autotest_click_menu_slot(0);
    inject_wait(10);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(0);
    inject_wait(90);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);

    rc = coop_menu_run();
    autotest_log("multiplayer menu returned %d, hosting %d", rc, coop_host_active() ? 1 : 0);

    if (rc != MAIN_MENU_NEW_GAME) {
        autotest_fail("HOST GAME, NEW GAME did not start a new game");
    }

    if (!coop_host_active() || !coop_is_enabled()) {
        autotest_fail("not hosting");
    }

    coop_menu_end_session();
    if (coop_host_active() || coop_is_enabled()) {
        autotest_fail("session did not end");
    }

    return 0;
}

// Client side of a nettest, joining through the menu (the address field
// comes filled in from [coop] host); the host leaves after a few seconds
// and the menu reports it. Paired with net_host.
static int coop_autotest_menu_join(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    inject_wait(10);
    coop_autotest_click_menu_slot(3);
    if (main_menu_loop() != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen");
        return -1;
    }

    // JOIN GAME: the server browser finds the host (LAN search) and lists
    // it first; COOP_AUTOTEST_JOIN_ADDRESS takes ENTER ADDRESS instead.
    coop_autotest_click_menu_slot(1);
    inject_wait(30);
    inject_call(coop_autotest_screenshot);
    if (getenv("COOP_AUTOTEST_JOIN_ADDRESS") != NULL) {
        coop_autotest_click_menu_slot(2);
        inject_wait(10);
        inject_key(KEY_RETURN);
    } else {
        coop_autotest_click_menu_slot(0);
    }
    // Playing until the host leaves, then the message.
    inject_wait(900);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);
    inject_wait(15);
    // BACK.
    coop_autotest_click_menu_slot(5);

    int rc = coop_menu_run();
    autotest_log("menu returned %d, last error '%s'", rc, coop_net_last_error());

    if (rc != -1) {
        autotest_fail("joining should come back to the main menu");
    }

    if (strstr(coop_net_last_error(), "left") == NULL) {
        autotest_fail("the host leaving was not reported");
    }

    if (coop_is_enabled() || coop_net_mode() != COOP_NET_NONE) {
        autotest_fail("co-op still on after leaving");
    }

    return 0;
}

// HOST GAME, NEW GAME, PRIVATE CODE: the game gets a relay code (needs
// [coop] relay), shown in the hosting message.
static int coop_autotest_menu_host_code(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    inject_wait(10);
    coop_autotest_click_menu_slot(3);
    if (main_menu_loop() != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen");
        return -1;
    }

    coop_autotest_click_menu_slot(0); // HOST GAME
    coop_autotest_click_menu_slot(0); // NEW GAME
    inject_wait(10);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(2); // PRIVATE CODE
    inject_wait(90);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);

    int rc = coop_menu_run();
    autotest_log("menu returned %d, relay code '%s'", rc, coop_net_relay_code());
    if (rc != MAIN_MENU_NEW_GAME || coop_net_relay_code()[0] == '\0') {
        autotest_fail("PRIVATE CODE did not host with a code");
    }
    coop_host_exit();
    return 0;
}

// A crash-report.txt from last time: opening MULTIPLAYER offers it, SEND
// REPORT sends it (to [coop] server) and it becomes crash-report-old.txt.
static int coop_autotest_menu_crash_report(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    std::string report;
    if (!coop_crash_pending(&report)) {
        autotest_fail("no crash-report.txt to offer");
        return -1;
    }

    inject_wait(10);
    coop_autotest_click_menu_slot(3); // MULTIPLAYER
    if (main_menu_loop() != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen");
        return -1;
    }

    inject_wait(20);
    inject_call(coop_autotest_screenshot); // "The game crashed last time."
    inject_key(KEY_RETURN);
    inject_wait(20);
    inject_call(coop_autotest_screenshot); // SEND REPORT / DON'T SEND
    coop_autotest_click_menu_slot(0);
    inject_wait(60);
    inject_call(coop_autotest_screenshot); // "Thank you!"
    inject_key(KEY_RETURN);
    inject_wait(20);
    coop_autotest_click_menu_slot(5); // BACK

    coop_menu_run();

    std::string again;
    autotest_log("after sending: pending %d", coop_crash_pending(&again) ? 1 : 0);
    if (coop_crash_pending(&again)) {
        autotest_fail("the crash report is still pending");
    }
    return 0;
}

// UPDATE (a newer version is offered and downloaded) and REPORT BUG (sent)
// against tools/coop_server.py, set as [coop] server.
static int coop_autotest_menu_server(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    inject_wait(10);
    coop_autotest_click_menu_slot(3);
    if (main_menu_loop() != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen");
        return -1;
    }

    // UPDATE: "version available" message, DOWNLOAD, "saved" message.
    coop_autotest_click_menu_slot(3);
    inject_wait(60);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);
    inject_wait(15);
    coop_autotest_click_menu_slot(0);
    inject_wait(60);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);
    inject_wait(15);

    // REPORT BUG: describe, send, "thank you".
    coop_autotest_click_menu_slot(4);
    inject_wait(10);
    for (const char* p = "trade crash"; *p != '\0'; p++) {
        inject_key(*p);
        inject_wait(2);
    }
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);
    inject_wait(60);
    inject_call(coop_autotest_screenshot);
    inject_key(KEY_RETURN);
    inject_wait(15);
    coop_autotest_click_menu_slot(5);

    int rc = coop_menu_run();
    autotest_log("menu returned %d", rc);
    return 0;
}

// The first item proto named `name`, or -1.
static int coop_autotest_item_pid(const char* name)
{
    for (int index = 1; index < 1000; index++) {
        Proto* proto;
        if (proto_ptr(index, &proto) != -1 && compat_stricmp(proto_name(index), name) == 0) {
            return index;
        }
    }
    return -1;
}

// Gives `critter` (a player) the first of `names` that exists and wields or
// wears it.
static void coop_autotest_equip(PlayerState* player, const char* const* names, int count, int hand)
{
    for (int index = 0; index < count; index++) {
        int pid = coop_autotest_item_pid(names[index]);
        Object* item;
        if (pid == -1 || obj_pid_new(&item, pid) == -1) {
            continue;
        }

        ActivePlayerScope scope(player);
        if (item_add_force(obj_dude, item, 1) == 0) {
            obj_disconnect(item, NULL);
            inven_wield(obj_dude, item, hand);
            autotest_log("%s gets %s", critter_name(obj_dude), names[index]);
        }
        return;
    }
}

// COOP_AUTOTEST_VIDEO=1: screenshots become video frames,
// vid-<wall clock ms>.raw (width, height, palette, pixels), so the frames
// of the host and the client can be matched up afterwards.
static int coop_autotest_video_dump(int width, int height, unsigned char* buffer, unsigned char* palette)
{
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                       .count();
    char name[64];
    snprintf(name, sizeof(name), "vid-%lld.raw", ms);
    FILE* stream = fopen(name, "wb");
    if (stream == NULL) {
        return -1;
    }
    fwrite(&width, sizeof(width), 1, stream);
    fwrite(&height, sizeof(height), 1, stream);
    fwrite(palette, 1, 768, stream);
    fwrite(buffer, 1, width * height, stream);
    fclose(stream);
    return 0;
}

static bool coop_autotest_video()
{
    return getenv("COOP_AUTOTEST_VIDEO") != NULL;
}

// Player 1 strolls to a free tile a few hexes away.
static void coop_autotest_stroll(Object* critter)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        int tile = tile_num_in_direction(critter->tile, roll_random(0, 5), roll_random(3, 7));
        if (tile != critter->tile && obj_blocking_at(NULL, tile, critter->elevation) == NULL
            && make_path_func(critter, critter->tile, tile, NULL, 0, obj_blocking_at) != 0) {
            register_begin(ANIMATION_REQUEST_RESERVED);
            register_object_move_to_tile(critter, tile, critter->elevation, -1, 0);
            register_end();
            return;
        }
    }
}

// COOP_AUTOTEST_TOUR=ALL: every map. The players are healed all the time,
// player 1 answers dialogs (first option) and in fights clears the
// opposing side and ends combat, so the tour gets through hostile places;
// it looks for crashes and hangs, not at the story.
static bool coop_autotest_rough = false;

static void coop_autotest_log_story(const char* when)
{
    if (const char* itemName = getenv("COOP_AUTOTEST_P2_ITEM")) {
        int pid = coop_autotest_item_pid(itemName);
        autotest_log("story %s: %s carried by p1 %d, p2 %d", when, itemName,
            inven_pid_quantity_carried(coop_player(0)->obj, pid),
            coop_player_count() > 1 ? inven_pid_quantity_carried(coop_player(1)->obj, pid) : 0);
    }
    autotest_log("story %s: vault_water %d find_chip %d chip_taken %d vats_blown %d master_blown %d master_dead %d vats_status %d",
        when, game_get_global_var(GVAR_VAULT_WATER), game_get_global_var(GVAR_FIND_WATER_CHIP),
        game_get_global_var(GVAR_NECROP_WATER_CHIP_TAKEN), game_get_global_var(GVAR_VATS_BLOWN),
        game_get_global_var(GVAR_MASTER_BLOWN), game_get_global_var(GVAR_MASTER_DEAD), game_get_global_var(GVAR_VATS_STATUS));
}

static void coop_autotest_heal_players()
{
    static bool tough = false;
    if (!tough) {
        // Hard to kill in one round, so the tour gets everywhere.
        tough = true;
        for (int index = 0; index < coop_player_count(); index++) {
            stat_set_bonus(coop_player(index)->obj, STAT_MAXIMUM_HIT_POINTS, 800); // (the HUD shows 3 digits)
        }
    }

    for (int index = 0; index < coop_player_count(); index++) {
        Object* obj = coop_player(index)->obj;
        if (obj != NULL && !critter_is_dead(obj)) {
            int missing = stat_level(obj, STAT_MAXIMUM_HIT_POINTS) - critter_get_hits(obj);
            if (missing > 0) {
                critter_adjust_hits(obj, missing);
            }
            // And no radiation sickness (the Glow otherwise kills a
            // player a few maps later): no rads, and no sickness already
            // on its way.
            int rads = critter_get_rads(obj);
            if (rads > 0) {
                ActivePlayerScope scope(coop_player(index));
                critter_adjust_rads(obj, -rads);
                queue_remove_this(obj, EVENT_TYPE_RADIATION);
            }
        }
    }
}

static bool coop_autotest_rough_turn(Object* player)
{
    coop_autotest_heal_players();
    if (player != coop_player(0)->obj) {
        // Player 2 plays (or the turn timer ends the turn).
        return false;
    }

    // Collect first: killing moves objects around in the object list.
    int team = player->data.critter.combat.team;
    std::vector<Object*> targets;
    for (Object* obj = obj_find_first_at(player->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && coop_player_of(obj) == NULL && !critter_is_dead(obj)
            && obj->data.critter.combat.team != team && obj_dist(obj, player) <= 20) {
            targets.push_back(obj);
        }
    }
    for (Object* obj : targets) {
        critter_kill(obj, -1, true);
    }
    if (!targets.empty()) {
        autotest_log("%s: cleared %d hostile critters", map_data.name, (int)targets.size());
    }
    combat_end();
    return true;
}

// A background process (runs in every input loop, dialogs too); an
// injected chain would never let player 2's part of a frame finish.
static void coop_autotest_answer_dialogs()
{
    static int presses = 0;
    static unsigned int last = 0;
    if (dialog_active() && elapsed_time(last) > 1000 && !inject_pending()) {
        // First option a few times; then Escape (a barter screen, say).
        inject_key(++presses % 4 == 0 ? KEY_ESCAPE : '1');
        last = get_time();
    }
}

// One frame of the main loop, as the game runs it while hosting.
static void coop_autotest_host_frame()
{
    sharedFpsLimiter.mark();
    coop_host_begin_main_input();
    int keyCode = get_input();
    coop_host_end_main_input();
    game_handle_input(keyCode, false);
    scripts_check_state();
    coop_process_requests();
    map_check_state();
    coop_host_frame();
    renderPresent();
    sharedFpsLimiter.throttle();
}

// Network tour, host side: both players (equipped) travel through towns;
// each stop runs a few seconds of the real game with the client walking
// around, with screenshots on both sides. Paired with net_tour_client.
static int coop_autotest_net_tour_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    // Screenshots without "Saved screenshot." in the (shared) message box.
    register_screendump(KEY_F12, coop_autotest_video() ? coop_autotest_video_dump : NULL);

    PlayerState* p1 = coop_player(0);
    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    const char* armor1[] = { "Metal Armor", "Leather Armor" };
    const char* weapon1[] = { "Hunting Rifle", "10mm Pistol" };
    const char* armor2[] = { "Leather Jacket", "Leather Armor" };
    const char* weapon2[] = { "10mm SMG", "10mm Pistol" };
    coop_autotest_equip(p1, armor1, 2, 0);
    coop_autotest_equip(p1, weapon1, 2, HAND_RIGHT);
    coop_autotest_equip(p2, armor2, 2, 0);
    coop_autotest_equip(p2, weapon2, 2, HAND_RIGHT);

    // COOP_AUTOTEST_P2_ITEM: a (quest) item in player 2's pack.
    if (const char* itemName = getenv("COOP_AUTOTEST_P2_ITEM")) {
        int pid = coop_autotest_item_pid(itemName);
        Object* item;
        if (pid != -1 && obj_pid_new(&item, pid) == 0 && item_add_force(p2->obj, item, 1) == 0) {
            obj_disconnect(item, NULL);
            autotest_log("player 2 carries %s (pid %d)", itemName, pid);
        } else {
            autotest_fail("no item %s", itemName);
        }
    }
    // COOP_AUTOTEST_STORY=VATS,MASTER: the story as if they were destroyed.
    if (const char* story = getenv("COOP_AUTOTEST_STORY")) {
        if (strstr(story, "VATS") != NULL) {
            game_set_global_var(GVAR_VATS_BLOWN, 1);
            game_set_global_var(GVAR_VATS_STATUS, 1);
        }
        if (strstr(story, "MASTER") != NULL) {
            game_set_global_var(GVAR_MASTER_BLOWN, 1);
            game_set_global_var(GVAR_MASTER_DEAD, 1);
        }
        if (strstr(story, "CHIP") != NULL) {
            game_set_global_var(GVAR_FIND_WATER_CHIP, 2);
        }
    }
    coop_autotest_log_story("start");

    coop_host_init();
    if (!coop_host_active()) {
        autotest_fail("cannot host");
        return -1;
    }
    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }

    unsigned int start = get_time();
    while (elapsed_time(start) < 30000 && !coop_net_client_connected()) {
        coop_autotest_host_frame();
    }

    if (!coop_net_client_connected()) {
        autotest_fail("no client joined");
        return -1;
    }

    const char* stops[] = {
        "SHADYE.MAP",
        "SHADYW.MAP",
        "JUNKENT.MAP",
        "JUNKCSNO.MAP",
        "HUBENT.MAP",
        "HUBDWNTN.MAP",
        "HUBOLDTN.MAP",
        "BROHDENT.MAP",
        // (Not LAADYTUM: its guard's greeting waits for player 1.)
        "LAFOLLWR.MAP",
        "HALLDED.MAP",
        "CHILDRN1.MAP",
    };

    // COOP_AUTOTEST_TOUR=A.MAP,B.MAP replaces the list (for screenshots),
    // ALL tours every map (see coop_autotest_rough).
    std::vector<std::string> tour;
    const char* tourList = getenv("COOP_AUTOTEST_TOUR");
    if (tourList != NULL && strcmp(tourList, "ALL") == 0) {
        coop_autotest_rough = true;
        // (Map 0 in a transition means the world map.)
        for (int index = 1; index < 500; index++) {
            char name[16];
            if (map_get_name_idx(name, index) != 0) {
                break;
            }
            tour.push_back(name);
        }
        // COOP_AUTOTEST_TOUR_AFTER=X.MAP: continue a tour after X.
        if (const char* after = getenv("COOP_AUTOTEST_TOUR_AFTER")) {
            auto it = std::find(tour.begin(), tour.end(), std::string(after));
            if (it != tour.end()) {
                tour.erase(tour.begin(), it + 1);
            }
        }
        coop_set_turn_hook(coop_autotest_rough_turn);
        add_bk_process(coop_autotest_answer_dialogs);
    } else if (const char* list = tourList) {
        if (getenv("COOP_AUTOTEST_ROUGH") != NULL) {
            coop_autotest_rough = true;
            coop_set_turn_hook(coop_autotest_rough_turn);
            add_bk_process(coop_autotest_answer_dialogs);
        }
        std::string rest = list;
        size_t comma;
        while ((comma = rest.find(',')) != std::string::npos) {
            tour.push_back(rest.substr(0, comma));
            rest = rest.substr(comma + 1);
        }
        tour.push_back(rest);
    } else {
        tour.assign(std::begin(stops), std::end(stops));
    }

    // COOP_AUTOTEST_SEED=n: the maps in a shuffled order (the same n gives
    // the same order, to repeat a run).
    if (const char* seed = getenv("COOP_AUTOTEST_SEED")) {
        std::mt19937 random((unsigned int)strtoul(seed, NULL, 10));
        std::shuffle(tour.begin(), tour.end(), random);
        autotest_log("tour order from seed %s, first %s", seed, tour.empty() ? "-" : tour.front().c_str());
    }

    int visited = 0;
    for (const std::string& stopName : tour) {
        const char* stop = stopName.c_str();
        int index = map_match_map_name(stop);
        if (index == -1) {
            autotest_log("no map %s", stop);
            continue;
        }

        if (coop_autotest_video()) {
            // Daylight for the video: noon (game time is in 1/10 s).
            set_game_time(game_time() / 864000 * 864000 + 12 * 36000);
        }

        MapTransition transition;
        transition.map = index;
        transition.elevation = 0;
        transition.tile = -1;
        transition.rotation = 0;
        map_leave_map(&transition);

        start = get_time();
        map_check_state();
        autotest_log("%s loaded in %u ms", stop, elapsed_time(start));

        // COOP_AUTOTEST_TALK=<name>: player 1 talks to them (for
        // screenshots; the dialog waits for player 1, so the tour ends by
        // timeout there).
        if (const char* talkTo = getenv("COOP_AUTOTEST_TALK")) {
            for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && strstr(critter_name(obj), talkTo) != NULL) {
                    if (obj->elevation != obj_dude->elevation) {
                        // Up or down to them first (player 2 follows).
                        obj_move_to_tile(obj_dude, obj->tile, obj->elevation, NULL);
                        map_set_elevation(obj->elevation);
                    }
                    // Somewhere they can be talked to from (not behind a
                    // counter).
                    for (int tile = 0; tile < HEX_GRID_SIZE; tile++) {
                        int distance = tile_dist(tile, obj->tile);
                        if (distance >= 1 && distance <= 3 && obj_blocking_at(NULL, tile, obj->elevation) == NULL
                            && make_path_func(obj_dude, tile, obj->tile, NULL, 0, obj_sight_blocking_at) != 0) {
                            obj_move_to_tile(obj_dude, tile, obj->elevation, NULL);
                            break;
                        }
                    }
                    obj_attempt_placement(coop_player(1)->obj, obj_dude->tile, obj->elevation, 2);
                    tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
                    for (int frame = 0; frame < 60; frame++) {
                        coop_autotest_host_frame();
                    }
                    gdialog_enter(obj, 1);
                    break;
                }
            }
        }

        // COOP_AUTOTEST_KILL=<name>: someone dies (the Master, say), with
        // the players on their level, to run what the story does then.
        if (getenv("COOP_AUTOTEST_LIST") != NULL) {
            for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
                    autotest_log("%s: critter '%s' pid %x elev %d", map_data.name, critter_name(obj), obj->pid, obj->elevation);
                }
            }
        }
        if (const char* killName = getenv("COOP_AUTOTEST_KILL")) {
            // The exact name first ("Master", not "Master's Pet").
            Object* victim = NULL;
            for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && !critter_is_dead(obj) && strcmp(critter_name(obj), killName) == 0) {
                    victim = obj;
                    break;
                }
            }
            for (Object* obj = obj_find_first(); obj != NULL && victim == NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && !critter_is_dead(obj) && strstr(critter_name(obj), killName) != NULL) {
                    victim = obj;
                }
            }
            for (Object* obj = victim; obj != NULL; obj = NULL) {
                {
                    if (obj->elevation != obj_dude->elevation) {
                        obj_move_to_tile(obj_dude, obj->tile, obj->elevation, NULL);
                        map_set_elevation(obj->elevation);
                    }
                    autotest_log("%s: killing %s", map_data.name, critter_name(obj));
                    critter_kill(obj, -1, true);
                    // As a fight would: their death script, killed by player 1.
                    if (obj->sid != -1) {
                        scr_set_objs(obj->sid, obj_dude, NULL);
                        exec_script_proc(obj->sid, SCRIPT_PROC_DESTROY);
                    }
                    break;
                }
            }
        }

        // COOP_AUTOTEST_RECRUIT=<name>: they join the party (as after their
        // dialog), then travel along.
        if (const char* recruitName = getenv("COOP_AUTOTEST_RECRUIT")) {
            for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && !critter_is_dead(obj) && strcmp(critter_name(obj), recruitName) == 0 && !isPartyMember(obj)) {
                    obj_attempt_placement(obj, obj_dude->tile, obj_dude->elevation, 2);
                    partyMemberAdd(obj);
                    autotest_log("%s: %s joins the party", map_data.name, recruitName);
                    break;
                }
            }
        }

        // COOP_AUTOTEST_STAY_MS: longer on each map (countdowns...).
        const char* stayText = getenv("COOP_AUTOTEST_STAY_MS");
        unsigned int stay = stayText != NULL ? (unsigned int)atoi(stayText) : (coop_autotest_rough ? 6000u : 9000u);

        start = get_time();
        bool shot = false;
        unsigned int lastFrame = 0;
        unsigned int lastStroll = 0;
        while (elapsed_time(start) < stay && coop_net_client_connected()) {
            if (coop_autotest_rough) {
                coop_autotest_heal_players();
            }
            coop_autotest_host_frame();
            if (coop_autotest_video()) {
                if (elapsed_time(lastFrame) >= 100) {
                    lastFrame = get_time();
                    dump_screen();
                }
                if (elapsed_time(lastStroll) >= 2500) {
                    lastStroll = get_time();
                    coop_autotest_stroll(obj_dude);
                }
            } else if (!shot && elapsed_time(start) > 6000) {
                dump_screen();
                shot = true;
            }
        }

        // Rough tour: the other elevations too.
        for (int elevation = 1; coop_autotest_rough && elevation < ELEVATION_COUNT && coop_net_client_connected(); elevation++) {
            if (map_is_elevation_empty(elevation) || obj_dude->elevation == elevation) {
                continue;
            }
            obj_move_to_tile(obj_dude, obj_dude->tile, elevation, NULL);
            map_set_elevation(elevation);
            unsigned int elevationStart = get_time();
            while (elapsed_time(elevationStart) < 4000 && coop_net_client_connected()) {
                coop_autotest_heal_players();
                coop_autotest_host_frame();
            }
            autotest_log("%s elevation %d: p2 elevation %d", map_data.name, elevation, coop_player(1)->obj->elevation);
        }

        if (!coop_net_client_connected()) {
            autotest_fail("client lost at %s", stop);
            break;
        }

        coop_autotest_log_story(map_data.name);

        if (const char* recruitName = getenv("COOP_AUTOTEST_RECRUIT")) {
            bool along = false;
            for (Object* obj = obj_find_first(); obj != NULL; obj = obj_find_next()) {
                if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && strcmp(critter_name(obj), recruitName) == 0 && isPartyMember(obj)) {
                    along = true;
                    autotest_log("%s: %s here, elev %d, %d from player 1", map_data.name, recruitName, obj->elevation, obj_dist(obj, obj_dude));
                }
            }
            if (!along) {
                autotest_log("%s: %s is not with the party", map_data.name, recruitName);
            }
        }

        // COOP_AUTOTEST_SAVELOAD: save and load on every map, player 2
        // connected.
        if (getenv("COOP_AUTOTEST_SAVELOAD") != NULL) {
            lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
            bool saved = SaveGame(LOAD_SAVE_MODE_QUICK) == 1;
            lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
            bool loaded = saved && LoadGame(LOAD_SAVE_MODE_QUICK) == 1;
            autotest_log("%s: save %d load %d, %d players, connected %d", map_data.name, saved ? 1 : 0, loaded ? 1 : 0,
                coop_player_count(), coop_net_client_connected() ? 1 : 0);
            if (!loaded || coop_player_count() != 2) {
                autotest_fail("%s: save/load failed", map_data.name);
            }
            for (int frame = 0; frame < 120; frame++) {
                coop_autotest_host_frame();
            }
        }
        // (Fetched only now: loading a save replaces the objects.)
        Object* p1obj = p1->obj;
        Object* p2obj = coop_player(1)->obj;
        autotest_log("%s: p1 tile %d elev %d hp %d rads %d, p2 tile %d elev %d hp %d rads %d, distance %d",
            map_data.name, p1obj->tile, p1obj->elevation, critter_get_hits(p1obj), critter_get_rads(p1obj),
            p2obj->tile, p2obj->elevation, critter_get_hits(p2obj), critter_get_rads(p2obj), obj_dist(p1obj, p2obj));

        if (coop_player_count() != 2 || p2obj->elevation != p1obj->elevation || p2obj->tile == -1) {
            autotest_fail("%s: players separated", stop);
        }
        if (coop_any_player_has(DAM_DEAD)) {
            autotest_fail("%s: a player died", stop);
            break;
        }
        visited++;
    }

    autotest_log("visited %d maps", visited);
    coop_host_exit();
    return 0;
}

static void coop_autotest_tour_walk()
{
    // Walk somewhere around player 2, look, again - forever (the host
    // ends the tour).
    static int step = 0;
    static const int offsets[][2] = { { 90, 40 }, { -80, 50 }, { 60, -60 }, { -70, -30 } };
    int offset[2] = { offsets[step % 4][0], offsets[step % 4][1] };
    step++;
    // With COOP_AUTOTEST_SEED: somewhere random around player 2 instead.
    if (const char* seed = getenv("COOP_AUTOTEST_SEED")) {
        static std::mt19937 random((unsigned int)strtoul(seed, NULL, 10) + 1);
        offset[0] = (int)(random() % 241) - 120;
        offset[1] = (int)(random() % 161) - 80;
    }
    coop_autotest_click_centered(320 + offset[0], 190 + offset[1]);
    if (coop_autotest_video()) {
        // A video frame about every 100 ms.
        for (int frame = 0; frame < 30; frame++) {
            inject_wait(6);
            inject_call(coop_autotest_screenshot);
        }
    } else {
        inject_wait(150);
        inject_call(coop_autotest_screenshot);
        inject_wait(90);
    }
    inject_call(coop_autotest_tour_walk);
}

static int coop_autotest_net_tour_client(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_CLIENT) {
        autotest_fail("needs [coop]mode=client");
        return -1;
    }

    coop_autotest_relay_client_setup();

    if (coop_autotest_video()) {
        register_screendump(KEY_F12, coop_autotest_video_dump);
    }

    inject_wait(300);
    inject_call(coop_autotest_tour_walk);

    int rc = coop_net_client_run();
    autotest_log("client finished with %d, %s", rc, coop_net_last_error());
    return 0;
}

// A rough session, host side: player 2 joins and walks, player 1 saves,
// player 2 drops out and comes back (keeping their character), player 1
// loads the save with player 2 connected, then a fight where player 2 does
// nothing and the turn timer ([coop]turn_time, HOST_EXTRA) ends their turns.
// Paired with net_session_client.

static int coop_autotest_level(PlayerState* player)
{
    ActivePlayerScope scope(player);
    return stat_pc_get(PC_STAT_LEVEL);
}

static int coop_autotest_session_p2_turns = 0;
static unsigned int coop_autotest_session_turn_start = 0;
static unsigned int coop_autotest_session_longest = 0;

static bool coop_autotest_session_turn(Object* player)
{
    if (coop_autotest_session_turn_start != 0) {
        coop_autotest_session_longest = std::max(coop_autotest_session_longest, elapsed_time(coop_autotest_session_turn_start));
        coop_autotest_session_turn_start = 0;
    }

    if (player == coop_player(0)->obj) {
        return coop_autotest_attack_turn(player);
    }

    coop_autotest_session_p2_turns++;
    coop_autotest_session_turn_start = get_time();
    return false;
}

static bool coop_autotest_session_frames(unsigned int ms, bool untilConnected, bool untilDisconnected)
{
    unsigned int start = get_time();
    while (elapsed_time(start) < ms) {
        coop_autotest_host_frame();
        if (untilConnected && coop_net_client_connected()) {
            return true;
        }
        if (untilDisconnected && !coop_net_client_connected()) {
            return true;
        }
    }
    return false;
}

static int coop_autotest_net_session_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }

    PlayerState* p2 = coop_add_player("premade\\stealth.gcd");
    if (p2 == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }
    std::string p2Name = critter_name(p2->obj);

    coop_host_init();
    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }
    if (!coop_autotest_session_frames(30000, true, false)) {
        autotest_fail("no client joined");
        return -1;
    }
    autotest_log("client joined");

    // COOP_AUTOTEST_XP: both players level up while player 2 is connected
    // (they then open their character screen).
    if (const char* xp = getenv("COOP_AUTOTEST_XP")) {
        stat_pc_add_experience(atoi(xp));
        autotest_log("after %s XP: player 1 level %d, player 2 level %d", xp, coop_autotest_level(coop_player(0)), coop_autotest_level(coop_player(1)));
    }

    int p2Level = coop_autotest_level(p2);
    int startTile = p2->obj->tile;
    // (Up to 20 s: big screens make both games slower.)
    for (int wait = 0; wait < 40 && coop_player(1)->obj->tile == startTile; wait++) {
        coop_autotest_session_frames(500, false, false);
    }
    coop_autotest_session_frames(2000, false, false);
    autotest_log("player 2 walked from %d to %d", startTile, coop_player(1)->obj->tile);
    if (coop_player(1)->obj->tile == startTile) {
        autotest_fail("player 2 did not move");
    }

    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (SaveGame(LOAD_SAVE_MODE_QUICK) != 1) {
        autotest_fail("save with player 2 connected failed");
    }
    int savedTile = coop_player(1)->obj->tile;

    // Player 2 drops out (F10), the game goes on, player 2 comes back.
    if (!coop_autotest_session_frames(60000, false, true)) {
        autotest_fail("client did not leave");
        return -1;
    }
    autotest_log("client left");
    // (The test client comes back after two seconds.)
    coop_autotest_session_frames(300, false, false);
    if (coop_player_count() != 2) {
        autotest_fail("player 2 vanished when the client left");
    }
    if ((coop_player(1)->obj->flags & OBJECT_HIDDEN) == 0) {
        autotest_fail("player 2's character is still out while they are away");
    }

    if (!coop_autotest_session_frames(30000, true, false)) {
        autotest_fail("client did not come back");
        return -1;
    }
    coop_autotest_session_frames(2000, false, false);
    if ((coop_player(1)->obj->flags & OBJECT_HIDDEN) != 0 || obj_dist(coop_player(1)->obj, coop_player(0)->obj) > 5) {
        autotest_fail("player 2 did not come back next to player 1");
    }
    autotest_log("client back: %d players, player 2 '%s' level %d", coop_player_count(),
        critter_name(coop_player(1)->obj), coop_autotest_level(coop_player(1)));
    if (coop_player_count() != 2 || p2Name != critter_name(coop_player(1)->obj) || coop_autotest_level(coop_player(1)) != p2Level) {
        autotest_fail("player 2 changed after rejoining");
    }

    // Load with the client connected.
    lsgSetQuickSlot(COOP_AUTOTEST_SLOT);
    if (LoadGame(LOAD_SAVE_MODE_QUICK) != 1) {
        autotest_fail("load with player 2 connected failed");
        return -1;
    }
    coop_autotest_session_frames(3000, false, false);
    autotest_log("after load: %d players, connected %d, player 2 tile %d (saved %d)", coop_player_count(),
        coop_net_client_connected() ? 1 : 0, coop_player(1)->obj->tile, savedTile);
    if (coop_player_count() != 2 || !coop_net_client_connected() || p2Name != critter_name(coop_player(1)->obj)) {
        autotest_fail("load broke the session");
    }

    // Player 2 trades (COOP_AUTOTEST_MAP/MERCHANT for a real merchant):
    // the game must stand still meanwhile.
    coop_autotest_scene();
    coop_autotest_session_frames(1500, false, false);
    {
        Object* merchant = NULL;
        const char* merchantName = getenv("COOP_AUTOTEST_MERCHANT");
        for (Object* obj = obj_find_first(); obj != NULL && merchantName != NULL; obj = obj_find_next()) {
            if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER && strstr(critter_name(obj), merchantName) != NULL) {
                merchant = obj;
                break;
            }
        }
        if (merchant == NULL) {
            for (int index = 1; index < 400 && merchant == NULL; index++) {
                if (compat_stricmp(proto_name(0x1000000 | index), "Merchant") == 0 && obj_pid_new(&merchant, 0x1000000 | index) == 0) {
                    obj_attempt_placement(merchant, coop_player(1)->obj->tile, coop_player(1)->obj->elevation, 1);
                }
            }
        }

        if (merchant == NULL) {
            autotest_fail("no merchant");
        } else {
            obj_attempt_placement(coop_player(1)->obj, merchant->tile, merchant->elevation, 1);
            int gameTime = game_time();
            unsigned int barterStart = get_time();
            // Close it after a while, as player 2 would (unless the test
            // has player 2 leave in the middle of it).
            if (getenv("COOP_AUTOTEST_LEAVE_IN_BARTER") == NULL) {
                inject_wait(240);
                inject_key(KEY_ESCAPE);
            }
            coop_request_barter(coop_player(1), merchant);
            coop_autotest_host_frame();
            autotest_log("player 2 traded with %s for %u ms; game time moved %d ticks",
                critter_name(merchant), elapsed_time(barterStart), game_time() - gameTime);
            if (elapsed_time(barterStart) < 2000) {
                autotest_fail("the barter screen did not stay open");
            }
            if (game_time() - gameTime > 5) {
                autotest_fail("the game did not pause while player 2 traded");
            }
        }
    }
    coop_autotest_session_frames(1500, false, false);

    // A fight where player 2 never acts: the turn timer has to move on.
    Object* enemy = coop_autotest_spawn_enemy(coop_player(0)->obj);
    if (enemy == NULL) {
        return -1;
    }
    coop_autotest_enemy = enemy;
    coop_autotest_turns_total = 0;
    memset(coop_autotest_turns, 0, sizeof(coop_autotest_turns));
    coop_set_turn_hook(coop_autotest_session_turn);

    STRUCT_664980 attack;
    memset(&attack, 0, sizeof(attack));
    attack.attacker = coop_player(0)->obj;
    attack.defender = enemy;
    unsigned int fightStart = get_time();
    combat(&attack);
    coop_set_turn_hook(NULL);

    autotest_log("fight over after %u ms: enemy dead %d, player 2 turns %d, longest player 2 turn %u ms (limit %u)",
        elapsed_time(fightStart), critter_is_dead(enemy) ? 1 : 0, coop_autotest_session_p2_turns,
        coop_autotest_session_longest, coop_turn_time_limit());
    if (coop_autotest_session_p2_turns == 0) {
        autotest_fail("player 2 never had a turn");
    }
    if (coop_turn_time_limit() != 0 && coop_autotest_session_longest > coop_turn_time_limit() + 6000) {
        autotest_fail("the turn timer did not end player 2's turn");
    }

    coop_autotest_session_frames(2000, false, false);
    dump_screen();
    coop_host_exit();
    return 0;
}

static void coop_autotest_session_walk()
{
    static int step = 0;
    static const int offsets[][2] = { { 90, 40 }, { -80, 50 }, { 60, -60 }, { -70, -30 } };
    if (step >= 4 && getenv("COOP_AUTOTEST_LEAVE_IN_MENU") != NULL) {
        // Leave (F10) with a screen still open: the host must cope.
        inject_key(getenv("COOP_AUTOTEST_LEAVE_IN_MENU")[0]);
        inject_wait(90);
        inject_call(coop_autotest_screenshot);
        inject_key(KEY_F10);
        return;
    }
    if (step >= 4) {
        // Player 2's own screens, each closed again, then leave (F10).
        const int keys[] = { 'i', 'c', 'p', 's' };
        for (int key : keys) {
            inject_key(key);
            inject_wait(90);
            inject_call(coop_autotest_screenshot);
            inject_key(KEY_ESCAPE);
            inject_wait(60);
        }
        inject_call(coop_autotest_screenshot);
        inject_key(KEY_F10);
        return;
    }
    const int* offset = offsets[step++ % 4];
    coop_autotest_click_centered(320 + offset[0], 190 + offset[1]);
    inject_wait(90);
    inject_call(coop_autotest_session_walk);
}

static int coop_autotest_net_session_client(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_CLIENT) {
        autotest_fail("needs [coop]mode=client");
        return -1;
    }

    coop_autotest_relay_client_setup();

    inject_wait(240);
    inject_call(coop_autotest_session_walk);
    int rc = coop_net_client_run();
    autotest_log("first visit finished with %d: %s", rc, coop_net_last_error());

    SDL_Delay(2000);

    // Back in; then just watch (no input, a screenshot now and then)
    // until the host ends.
    if (const char* leaveAfter = getenv("COOP_AUTOTEST_LEAVE_IN_BARTER")) {
        inject_wait(atoi(leaveAfter));
        inject_call(coop_autotest_screenshot);
        inject_key(KEY_F10);
    }
    inject_call(coop_autotest_session_shots);
    rc = coop_net_client_run();
    autotest_log("second visit finished with %d: %s", rc, coop_net_last_error());
    return 0;
}

// The end of the game with player 2 connected: the ending slideshow and
// movie (which ends the game, like main_game_loop would). Paired with
// net_tour_client, whose screenshots show what player 2 sees.
static int coop_autotest_net_ending_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }

    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }
    if (coop_add_player("premade\\stealth.gcd") == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    coop_host_init();
    if (!coop_autotest_relay_host_setup()) {
        return -1;
    }
    if (!coop_autotest_session_frames(30000, true, false)) {
        autotest_fail("no client joined");
        return -1;
    }
    coop_autotest_session_frames(3000, false, false);

    // The good ending's choices.
    game_set_global_var(GVAR_VATS_STATUS, 1);
    game_set_global_var(GVAR_NECROP_WATER_CHIP_TAKEN, 1);

    unsigned int start = get_time();
    endgame_slideshow();
    autotest_log("slideshow took %u ms, client connected %d", elapsed_time(start), coop_net_client_connected() ? 1 : 0);

    start = get_time();
    endgame_movie();
    autotest_log("movie and credits took %u ms, game ends %d, client connected %d", elapsed_time(start),
        game_user_wants_to_quit, coop_net_client_connected() ? 1 : 0);
    if (game_user_wants_to_quit != 2) {
        autotest_fail("the ending did not end the game");
    }
    if (!coop_net_client_connected()) {
        autotest_fail("player 2 was dropped during the ending");
    }

    coop_host_exit();
    return 0;
}

static void coop_autotest_save_surface()
{
    // The movie player draws straight to the screen surface, which
    // dump_screen() does not see.
    static int count = 0;
    char name[32];
    snprintf(name, sizeof(name), "surface%02d.bmp", count++);
    SDL_SaveBMP(gSdlSurface, name);
}

// The ending movie on its own, with screenshots while it plays (to tell
// the movie player's own picture from what a client gets).
static int coop_autotest_movie(AutotestNewGameProc* newGame)
{
    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }
    for (int shot = 0; shot < 6; shot++) {
        inject_wait(90);
        inject_call(coop_autotest_save_surface);
    }
    gmovie_play(MOVIE_WALKM, 0);
    autotest_log("movie done");
    return 0;
}

// MULTIPLAYER, MORE: COLORBLIND toggles and is saved.
static int coop_autotest_menu_more(AutotestNewGameProc* newGame)
{
    loadColorTable("color.pal");
    palette_fade_to(cmap);
    main_menu_create();
    main_menu_show(false);

    inject_wait(10);
    coop_autotest_click_menu_slot(3);
    if (main_menu_loop() != MAIN_MENU_MULTIPLAYER) {
        autotest_fail("MULTIPLAYER not chosen");
        return -1;
    }

    inject_wait(10);
    inject_call(coop_autotest_screenshot); // the MULTIPLAYER menu
    coop_autotest_click_menu_slot(2); // MORE
    inject_wait(20);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(1); // OUTLINES
    inject_wait(20);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(0); // PLAYER 1: GREEN -> WHITE (skips yellow)
    inject_wait(20);
    coop_autotest_click_menu_slot(1); // PLAYER 2: YELLOW -> ORANGE
    inject_wait(20);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(2); // BACK
    inject_wait(10);
    coop_autotest_click_menu_slot(2); // RESOLUTION
    inject_wait(20);
    coop_autotest_click_menu_slot(0); // 640x480 -> 800x600
    inject_wait(20);
    coop_autotest_click_menu_slot(0); // -> 1024x768
    inject_wait(20);
    inject_call(coop_autotest_screenshot);
    coop_autotest_click_menu_slot(1); // BACK
    inject_wait(20);
    inject_call(coop_autotest_screenshot); // "Restart the game"
    inject_key(KEY_RETURN);
    inject_wait(20);
    coop_autotest_click_menu_slot(3); // BACK
    inject_wait(10);
    coop_autotest_click_menu_slot(5); // BACK

    coop_menu_run();

    Config resolutionConfig;
    int width = 0;
    int height = 0;
    if (config_init(&resolutionConfig)) {
        config_load(&resolutionConfig, "f1_res.ini", false);
        config_get_value(&resolutionConfig, "MAIN", "SCR_WIDTH", &width);
        config_get_value(&resolutionConfig, "MAIN", "SCR_HEIGHT", &height);
        config_exit(&resolutionConfig);
    }
    // Two steps from 640x480 (or whatever the test started with).
    autotest_log("f1_res.ini after RESOLUTION: %dx%d (screen %dx%d)", width, height, screenGetWidth(), screenGetHeight());
    if (screenGetWidth() == 640 ? (width != 1024 || height != 768) : (width == screenGetWidth() && height == screenGetHeight())) {
        autotest_fail("RESOLUTION did not save a new size");
    }

    int colors[2];
    coop_outline_colors_from_config(colors);
    int expected[2] = { 2, 3 };
    coop_set_outline_colors(0, expected);
    int white = coop_viewer_outline_color(0);
    autotest_log("outline colors after menu: %s, %s", coop_outline_color_name(colors[0]), coop_outline_color_name(colors[1]));
    if (colors[0] != 2 || colors[1] != 3 || white != colorTable[(31 << 10) | (31 << 5) | 31]) {
        autotest_fail("OUTLINES did not change the colors");
    }
    return 0;
}

static void coop_autotest_screenshot_every_2s()
{
    inject_wait(120);
    inject_call(coop_autotest_screenshot);
    inject_call(coop_autotest_screenshot_every_2s);
}

// The credits from the main menu, with a screenshot every two seconds
// (the co-op lines come at the end).
static int coop_autotest_credits(AutotestNewGameProc* newGame)
{
    inject_call(coop_autotest_screenshot_every_2s);
    credits("credits.txt", -1, false);
    autotest_log("credits done");
    return 0;
}

static Object* coop_autotest_death_victim = NULL;

static void coop_autotest_kill_victim()
{
    if (coop_autotest_death_victim != NULL) {
        autotest_log("%s dies", critter_name(coop_autotest_death_victim));
        critter_kill(coop_autotest_death_victim, -1, true);
    }
}

// A player dies while player 2 is connected (COOP_AUTOTEST_VICTIM=1 or 2):
// the game's own main loop must end the game in death. Paired with
// net_tour_client.
static int coop_autotest_net_death_host(AutotestNewGameProc* newGame)
{
    if (coop_net_mode() != COOP_NET_HOST) {
        autotest_fail("needs [coop]mode=host");
        return -1;
    }
    if (!autotest_new_game(newGame, "premade\\combat.gcd")) {
        return -1;
    }
    if (coop_add_player("premade\\stealth.gcd") == NULL) {
        autotest_fail("coop_add_player failed");
        return -1;
    }

    coop_host_init();
    if (!coop_autotest_session_frames(30000, true, false)) {
        autotest_fail("no client joined");
        return -1;
    }

    const char* victim = getenv("COOP_AUTOTEST_VICTIM");
    int index = victim != NULL && atoi(victim) == 1 ? 0 : 1;
    coop_autotest_death_victim = coop_player(index)->obj;

    // In the real loop: a few seconds of play, then the death.
    inject_wait(240);
    inject_call(coop_autotest_kill_victim);

    autotest_log("before the loop: quit %d", game_user_wants_to_quit);
    unsigned int start = get_time();
    bool death = main_game_loop_for_test();
    autotest_log("after the loop: quit %d, victim dead %d", game_user_wants_to_quit, critter_is_dead(coop_autotest_death_victim) ? 1 : 0);
    autotest_log("main loop ended after %u ms, death %d, client connected %d", elapsed_time(start), death ? 1 : 0, coop_net_client_connected() ? 1 : 0);
    if (!death) {
        autotest_fail("the game did not end in death");
    }

    coop_host_exit();
    return 0;
}

} // namespace fallout
