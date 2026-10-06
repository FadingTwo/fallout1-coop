#include "game/coop_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "game/config.h"
#include "game/coop.h"
#include "game/coop_setup_font.h"
#include "game/gconfig.h"
#include "platform_compat.h"

namespace fallout {

#define SETUP_WIDTH 640
#define SETUP_HEIGHT 480
#define SETUP_MARGIN 24
#define SETUP_LINE 22

// Pip-Boy green.
#define SETUP_GREEN 0x33FF66
#define SETUP_DIM 0x1E9940

namespace fs = std::filesystem;

typedef struct SetupInstall {
    std::string path;
    std::string label;
} SetupInstall;

// -----------------------------------------------------------------------------
// Finding the game

// Finds `name` in `dir`, ignoring case (GOG and Steam ship upper case names).
static bool setup_find(const fs::path& dir, const char* name, fs::path* out)
{
    std::error_code error;
    for (fs::directory_iterator it(dir, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        if (compat_stricmp(it->path().filename().string().c_str(), name) == 0) {
            *out = it->path();
            return true;
        }
    }
    return false;
}

// Fallout 2 has a MASTER.DAT and CRITTER.DAT too (and many own both).
static bool setup_is_fallout2(const fs::path& dir)
{
    fs::path found;
    return setup_find(dir, "fallout2.exe", &found) || setup_find(dir, "patch000.dat", &found);
}

static bool setup_is_install(const fs::path& dir)
{
    fs::path master;
    fs::path critter;
    return setup_find(dir, "master.dat", &master) && setup_find(dir, "critter.dat", &critter) && !setup_is_fallout2(dir);
}

static void setup_add(std::vector<SetupInstall>* installs, const fs::path& dir, const char* label)
{
    std::error_code error;
    if (!fs::is_directory(dir, error) || !setup_is_install(dir)) {
        return;
    }

    fs::path canonical = fs::weakly_canonical(dir, error);
    std::string path = (error ? dir : canonical).string();
    for (const SetupInstall& install : *installs) {
        if (install.path == path) {
            return;
        }
    }

    installs->push_back({ path, label });
}

// Stores whose folder names we can't be sure of (Epic, Amazon): Fallout 1
// anywhere up to two folders below `root`.
static void setup_scan(std::vector<SetupInstall>* installs, const fs::path& root, const char* label, int depth = 2)
{
    std::error_code error;
    if (depth < 0 || !fs::is_directory(root, error)) {
        return;
    }
    setup_add(installs, root, label);
    for (fs::directory_iterator it(root, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        if (it->is_directory(error)) {
            setup_scan(installs, it->path(), label, depth - 1);
        }
    }
}

// Steam libraries listed in libraryfolders.vdf ("path" "...").
static void setup_add_steam_libraries(std::vector<SetupInstall>* installs, const fs::path& steam)
{
    setup_add(installs, steam / "steamapps" / "common" / "Fallout", "Steam");

    FILE* stream = fopen((steam / "steamapps" / "libraryfolders.vdf").string().c_str(), "rt");
    if (stream == NULL) {
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), stream) != NULL) {
        char* key = strstr(line, "\"path\"");
        if (key == NULL) {
            continue;
        }

        char* start = strchr(key + 6, '"');
        char* end = start != NULL ? strchr(start + 1, '"') : NULL;
        if (end == NULL) {
            continue;
        }

        std::string library(start + 1, end);
        // Escaped backslashes on Windows.
        std::string unescaped;
        for (size_t index = 0; index < library.size(); index++) {
            if (library[index] == '\\' && index + 1 < library.size() && library[index + 1] == '\\') {
                index++;
            }
            unescaped += library[index];
        }

        setup_add(installs, fs::path(unescaped) / "steamapps" / "common" / "Fallout", "Steam");
    }

    fclose(stream);
}

static std::vector<SetupInstall> setup_find_installs()
{
    std::vector<SetupInstall> installs;

    // Testing aid and power users: extra places to look.
    const char* extra = getenv("FALLOUT_SETUP_SEARCH");
    if (extra != NULL) {
        std::string list = extra;
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find(';', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            if (end > start) {
                setup_add(&installs, list.substr(start, end - start), "");
            }
            start = end + 1;
        }
    }

    setup_add(&installs, fs::current_path(), "this folder");

    char* basePath = SDL_GetBasePath();
    if (basePath != NULL) {
        setup_add(&installs, basePath, "next to the game");
        SDL_free(basePath);
    }

#ifdef _WIN32
    DWORD driveMask = GetLogicalDrives();
    for (int letter = 2; letter < 26; letter++) {
        if ((driveMask & (1u << letter)) == 0 || GetDriveTypeA((std::string(1, (char)('A' + letter)) + ":\\").c_str()) != DRIVE_FIXED) {
            continue;
        }
        std::string root = std::string(1, (char)('A' + letter)) + ":\\";
        setup_scan(&installs, root + "Program Files\\Epic Games", "Epic");
        setup_scan(&installs, root + "Epic Games", "Epic");
        setup_scan(&installs, root + "Amazon Games\\Library", "Amazon");
        setup_scan(&installs, root + "Program Files\\Amazon Games\\Library", "Amazon");
        setup_add(&installs, root + "GOG Games\\Fallout", "GOG");
        setup_add(&installs, root + "Program Files (x86)\\GOG Galaxy\\Games\\Fallout", "GOG");
        setup_add(&installs, root + "Program Files\\GOG Galaxy\\Games\\Fallout", "GOG");
        setup_add(&installs, root + "Games\\Fallout", "");
        setup_add(&installs, root + "Fallout", "");
        setup_add(&installs, root + "Program Files (x86)\\Interplay\\Fallout", "CD");
        setup_add_steam_libraries(&installs, root + "Program Files (x86)\\Steam");
        setup_add_steam_libraries(&installs, root + "Program Files\\Steam");
        setup_add_steam_libraries(&installs, root + "SteamLibrary");
    }
#else
    const char* home = getenv("HOME");
    if (home != NULL) {
        fs::path h = home;
        setup_add(&installs, h / "Games" / "Heroic" / "Fallout", "Heroic");
        setup_add(&installs, h / "GOG Games" / "Fallout", "GOG");
        setup_add(&installs, h / "Games" / "Fallout", "");
        // Lutris.
        setup_add(&installs, h / "Games" / "fallout", "Lutris");
        setup_add(&installs, h / "Games" / "gog" / "fallout", "Lutris");
        setup_add(&installs, h / "Games" / "GOG" / "Fallout", "GOG");
        setup_add(&installs, h / "Fallout", "");
        setup_add(&installs, h / ".wine" / "drive_c" / "GOG Games" / "Fallout", "Wine");
        setup_add(&installs, h / ".wine" / "drive_c" / "Program Files (x86)" / "GOG Galaxy" / "Games" / "Fallout", "Wine");
        setup_add_steam_libraries(&installs, h / ".steam" / "steam");
        setup_add_steam_libraries(&installs, h / ".local" / "share" / "Steam");
        setup_add_steam_libraries(&installs, h / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam");
        setup_add_steam_libraries(&installs, h / "Library" / "Application Support" / "Steam");
        // Heroic's Epic and Amazon installs.
        setup_scan(&installs, h / "Games" / "Heroic", "Heroic", 3);
    }
#endif

    return installs;
}

// Writes the paths of the installation in `dir` into fallout.cfg.
static bool setup_write_config(const char* configPath, const fs::path& dir)
{
    fs::path master;
    fs::path critter;
    fs::path data;
    if (!setup_find(dir, "master.dat", &master) || !setup_find(dir, "critter.dat", &critter)) {
        return false;
    }

    if (!setup_find(dir, "data", &data)) {
        // Saves and patches need it; the original installer creates it.
        std::error_code error;
        data = dir / "DATA";
        fs::create_directories(data, error);
    }

    fs::path sound;
    fs::path music;
    std::string musicPath;
    if (setup_find(data, "sound", &sound) && setup_find(sound, "music", &music)) {
        musicPath = music.string() + std::string(1, (char)fs::path::preferred_separator);
    }

    Config config;
    if (!config_init(&config)) {
        return false;
    }

    config_load(&config, configPath, false);
    config_set_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_DAT_KEY, master.string().c_str());
    config_set_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_DAT_KEY, critter.string().c_str());
    config_set_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_PATCHES_KEY, data.string().c_str());
    config_set_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_PATCHES_KEY, data.string().c_str());
    if (!musicPath.empty()) {
        config_set_string(&config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MUSIC_PATH1_KEY, musicPath.c_str());
        config_set_string(&config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MUSIC_PATH2_KEY, musicPath.c_str());
    }

    bool saved = config_save(&config, configPath, false);
    config_exit(&config);
    return saved;
}

// -----------------------------------------------------------------------------
// Terminal screen

typedef struct SetupScreen {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    unsigned int pixels[SETUP_WIDTH * SETUP_HEIGHT];
} SetupScreen;

static void setup_clear(SetupScreen* screen)
{
    for (int index = 0; index < SETUP_WIDTH * SETUP_HEIGHT; index++) {
        screen->pixels[index] = 0x020A04;
    }
}

static void setup_fill(SetupScreen* screen, int x, int y, int width, int height, unsigned int color)
{
    for (int row = std::max(y, 0); row < std::min(y + height, SETUP_HEIGHT); row++) {
        for (int column = std::max(x, 0); column < std::min(x + width, SETUP_WIDTH); column++) {
            screen->pixels[row * SETUP_WIDTH + column] = color;
        }
    }
}

// Draws `text` (truncated to `maxChars`); returns its width.
static int setup_text(SetupScreen* screen, int x, int y, const char* text, unsigned int color, int maxChars = 60)
{
    int drawn = 0;
    for (const char* p = text; *p != '\0' && drawn < maxChars; p++, drawn++) {
        int ch = (unsigned char)*p;
        if (ch < COOP_SETUP_FONT_FIRST || ch > COOP_SETUP_FONT_LAST) {
            ch = '?';
        }

        const unsigned short* glyph = coop_setup_font[ch - COOP_SETUP_FONT_FIRST];
        for (int row = 0; row < COOP_SETUP_FONT_HEIGHT; row++) {
            for (int column = 0; column < COOP_SETUP_FONT_WIDTH; column++) {
                if ((glyph[row] >> (COOP_SETUP_FONT_WIDTH - 1 - column)) & 1) {
                    int px = x + drawn * COOP_SETUP_FONT_WIDTH + column;
                    int py = y + row;
                    if (px >= 0 && px < SETUP_WIDTH && py >= 0 && py < SETUP_HEIGHT) {
                        screen->pixels[py * SETUP_WIDTH + px] = color;
                    }
                }
            }
        }
    }
    return drawn * COOP_SETUP_FONT_WIDTH;
}

// Paths under the home folder as ~/..., as in a terminal.
static std::string setup_display_path(const std::string& path)
{
#ifndef _WIN32
    const char* home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        std::string prefix = std::string(home);
        if (prefix.back() != '/') {
            prefix += '/';
        }
        if (path.compare(0, prefix.size(), prefix) == 0) {
            return "~/" + path.substr(prefix.size());
        }
    }
#endif
    return path;
}

// Long paths: keep the end, which tells them apart.
static std::string setup_shorten(const std::string& text, size_t maxChars)
{
    if (text.size() <= maxChars) {
        return text;
    }
    return "..." + text.substr(text.size() - (maxChars - 3));
}

// Phosphor glow and scan lines, then present.
static void setup_present(SetupScreen* screen)
{
    static unsigned int lit[SETUP_WIDTH * SETUP_HEIGHT];
    memcpy(lit, screen->pixels, sizeof(lit));

    for (int y = 1; y < SETUP_HEIGHT - 1; y++) {
        for (int x = 1; x < SETUP_WIDTH - 1; x++) {
            unsigned int pixel = lit[y * SETUP_WIDTH + x];
            if ((pixel & 0xFF00) > 0x8000) {
                continue;
            }
            int glow = 0;
            glow += (lit[y * SETUP_WIDTH + x - 1] & 0xFF00) > 0x8000;
            glow += (lit[y * SETUP_WIDTH + x + 1] & 0xFF00) > 0x8000;
            glow += (lit[(y - 1) * SETUP_WIDTH + x] & 0xFF00) > 0x8000;
            glow += (lit[(y + 1) * SETUP_WIDTH + x] & 0xFF00) > 0x8000;
            if (glow != 0) {
                unsigned int g = std::min(0x10 + glow * 0x14, 0x60);
                screen->pixels[y * SETUP_WIDTH + x] = (g / 4 << 16) | (g << 8) | (g / 3);
            }
        }
    }

    for (int y = 1; y < SETUP_HEIGHT; y += 2) {
        for (int x = 0; x < SETUP_WIDTH; x++) {
            unsigned int pixel = screen->pixels[y * SETUP_WIDTH + x];
            unsigned int r = ((pixel >> 16) & 0xFF) * 3 / 4;
            unsigned int g = ((pixel >> 8) & 0xFF) * 3 / 4;
            unsigned int b = (pixel & 0xFF) * 3 / 4;
            screen->pixels[y * SETUP_WIDTH + x] = (r << 16) | (g << 8) | b;
        }
    }

    SDL_UpdateTexture(screen->texture, NULL, screen->pixels, SETUP_WIDTH * 4);
    SDL_RenderClear(screen->renderer);
    SDL_RenderCopy(screen->renderer, screen->texture, NULL, NULL);
    SDL_RenderPresent(screen->renderer);
}

static void setup_save_screenshot(SetupScreen* screen, const char* path)
{
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(screen->pixels, SETUP_WIDTH, SETUP_HEIGHT, 32, SETUP_WIDTH * 4, SDL_PIXELFORMAT_RGB888);
    if (surface != NULL) {
        SDL_SaveBMP(surface, path);
        SDL_FreeSurface(surface);
    }
}

bool coop_setup_ensure_game_data(int argc, char** argv)
{
    // fallout.cfg, found the way gconfig_init() finds it.
    char configPath[COMPAT_MAX_PATH];
    char* sep = argc > 0 ? strrchr(argv[0], '\\') : NULL;
    if (sep != NULL) {
        *sep = '\0';
        snprintf(configPath, sizeof(configPath), "%s\\%s", argv[0], GAME_CONFIG_FILE_NAME);
        *sep = '\\';
    } else {
        strcpy(configPath, GAME_CONFIG_FILE_NAME);
    }

    Config config;
    if (!config_init(&config)) {
        return true;
    }

    config_load(&config, configPath, false);
    // Command line overrides, as the game applies them.
    config_cmd_line_parse(&config, argc, argv);

    char* master;
    char* critter;
    std::string masterPath = config_get_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_DAT_KEY, &master) ? master : "master.dat";
    std::string critterPath = config_get_string(&config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_DAT_KEY, &critter) ? critter : "critter.dat";
    config_exit(&config);

    FILE* masterStream = compat_fopen(masterPath.c_str(), "rb");
    FILE* critterStream = compat_fopen(critterPath.c_str(), "rb");
    bool found = masterStream != NULL && critterStream != NULL;
    if (masterStream != NULL) {
        fclose(masterStream);
    }
    if (critterStream != NULL) {
        fclose(critterStream);
    }

    if (found) {
        return true;
    }

    // Not found: the setup screen.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return true;
    }

    SetupScreen* screen = new SetupScreen();
    screen->window = SDL_CreateWindow("Fallout - Setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SETUP_WIDTH * 3 / 2, SETUP_HEIGHT * 3 / 2, SDL_WINDOW_RESIZABLE);
    screen->renderer = screen->window != NULL ? SDL_CreateRenderer(screen->window, -1, 0) : NULL;
    if (screen->renderer == NULL) {
        if (screen->window != NULL) {
            SDL_DestroyWindow(screen->window);
        }
        delete screen;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return true;
    }

    SDL_RenderSetLogicalSize(screen->renderer, SETUP_WIDTH, SETUP_HEIGHT);
    screen->texture = SDL_CreateTexture(screen->renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, SETUP_WIDTH, SETUP_HEIGHT);

    std::vector<SetupInstall> installs = setup_find_installs();

    // Entries: the installations found, then "type a path", then "quit".
    int typeIndex = (int)installs.size();
    int quitIndex = typeIndex + 1;
    int selected = 0;
    bool typing = installs.empty();
    std::string typed;
    std::string message;
    bool messageIsError = false;
    bool done = false;
    bool result = false;
    int frame = 0;

    const char* screenshot = getenv("FALLOUT_SETUP_SCREENSHOT");
    const char* choose = getenv("FALLOUT_SETUP_CHOOSE");

    if (typing) {
        SDL_StartTextInput();
    }

    // Where each entry's line is, for the mouse.
    const int listTop = SETUP_MARGIN + 5 * SETUP_LINE;

    while (!done) {
        int confirm = -1;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                done = true;
                break;
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
                if (!typing) {
                    int y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
                    int line = (y - listTop) / SETUP_LINE;
                    if (y >= listTop && line >= 0 && line <= quitIndex) {
                        selected = line;
                        if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                            confirm = line;
                        }
                    }
                }
                break;
            case SDL_TEXTINPUT:
                if (typing) {
                    typed += event.text.text;
                }
                break;
            case SDL_KEYDOWN:
                if (typing) {
                    SDL_Keycode key = event.key.keysym.sym;
                    if (key == SDLK_BACKSPACE && !typed.empty()) {
                        typed.pop_back();
                    } else if (key == SDLK_v && (event.key.keysym.mod & KMOD_CTRL) != 0) {
                        char* clipboard = SDL_GetClipboardText();
                        if (clipboard != NULL) {
                            typed += clipboard;
                            SDL_free(clipboard);
                        }
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        confirm = typeIndex;
                    } else if (key == SDLK_ESCAPE) {
                        if (installs.empty()) {
                            confirm = quitIndex;
                        } else {
                            typing = false;
                            SDL_StopTextInput();
                            message.clear();
                        }
                    }
                } else {
                    switch (event.key.keysym.sym) {
                    case SDLK_UP:
                        selected = (selected + quitIndex) % (quitIndex + 1);
                        break;
                    case SDLK_DOWN:
                        selected = (selected + 1) % (quitIndex + 1);
                        break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        confirm = selected;
                        break;
                    case SDLK_ESCAPE:
                        confirm = quitIndex;
                        break;
                    }
                }
                break;
            }
        }

        if (frame == 1 && choose != NULL) {
            confirm = atoi(choose);
        }

        if (confirm != -1) {
            if (confirm == quitIndex) {
                done = true;
            } else if (confirm == typeIndex && !typing) {
                typing = true;
                SDL_StartTextInput();
                message.clear();
            } else {
                // Trim what was typed or pasted.
                std::string path = confirm == typeIndex ? typed : installs[confirm].path;
                while (!path.empty() && (path.back() == ' ' || path.back() == '\n' || path.back() == '\r' || path.back() == '"')) {
                    path.pop_back();
                }
                while (!path.empty() && (path.front() == ' ' || path.front() == '"')) {
                    path.erase(path.begin());
                }

                std::error_code error;
                if (path.empty() || !fs::is_directory(path, error)) {
                    message = "That folder does not exist.";
                    messageIsError = true;
                } else if (setup_is_fallout2(path)) {
                    message = "That is Fallout 2. This mod needs Fallout 1.";
                    messageIsError = true;
                } else if (!setup_is_install(path)) {
                    message = "No MASTER.DAT and CRITTER.DAT in that folder.";
                    messageIsError = true;
                } else if (!setup_write_config(configPath, path)) {
                    message = "Could not write fallout.cfg here.";
                    messageIsError = true;
                } else {
                    message = "Fallout found. Starting...";
                    messageIsError = false;
                    result = true;
                    done = true;
                }
            }
        }

        // Draw.
        setup_clear(screen);
        setup_fill(screen, 0, 0, SETUP_WIDTH, SETUP_LINE + 6, SETUP_GREEN);
        setup_text(screen, SETUP_MARGIN, 4, "VAULT-TEC FALLOUT SETUP TERMINAL", 0x041A08);
        setup_text(screen, SETUP_WIDTH - SETUP_MARGIN - (int)strlen("CO-OP v" COOP_VERSION) * COOP_SETUP_FONT_WIDTH, 4, "CO-OP v" COOP_VERSION, 0x041A08);

        int y = SETUP_MARGIN + 2 * SETUP_LINE;
        if (installs.empty()) {
            setup_text(screen, SETUP_MARGIN, y, "No Fallout installation was found.", SETUP_GREEN);
        } else {
            setup_text(screen, SETUP_MARGIN, y, "Where is Fallout installed?", SETUP_GREEN);
        }
        y += SETUP_LINE;
        setup_text(screen, SETUP_MARGIN, y, "The folder with MASTER.DAT and CRITTER.DAT.", SETUP_DIM);

        y = listTop;
        for (int index = 0; index <= quitIndex; index++, y += SETUP_LINE) {
            std::string line;
            if (index < typeIndex) {
                std::string label = installs[index].label.empty() ? "" : "  (" + installs[index].label + ")";
                line = setup_shorten(setup_display_path(installs[index].path), 56 - label.size()) + label;
            } else if (index == typeIndex) {
                line = "[ TYPE OR PASTE A PATH ]";
            } else {
                line = "[ QUIT ]";
            }

            bool highlight = index == selected && !(typing && index != typeIndex);
            if (highlight) {
                setup_fill(screen, SETUP_MARGIN - 6, y - 1, SETUP_WIDTH - 2 * SETUP_MARGIN + 12, SETUP_LINE - 1, SETUP_GREEN);
                setup_text(screen, SETUP_MARGIN, y, ("> " + line).c_str(), 0x041A08, 58);
            } else {
                setup_text(screen, SETUP_MARGIN, y, ("  " + line).c_str(), SETUP_GREEN, 58);
            }
        }

        if (typing) {
            y += SETUP_LINE;
            setup_text(screen, SETUP_MARGIN, y, "PATH:", SETUP_GREEN);
            std::string shown = setup_shorten(typed, 52);
            int width = setup_text(screen, SETUP_MARGIN + 6 * COOP_SETUP_FONT_WIDTH, y, shown.c_str(), SETUP_GREEN);
            if ((SDL_GetTicks() / 400) % 2 == 0) {
                setup_fill(screen, SETUP_MARGIN + 6 * COOP_SETUP_FONT_WIDTH + width + 1, y + 2, COOP_SETUP_FONT_WIDTH - 2, COOP_SETUP_FONT_HEIGHT - 4, SETUP_GREEN);
            }
        }

        if (!message.empty()) {
            std::string text = (messageIsError ? "! " : "") + message;
            setup_text(screen, SETUP_MARGIN, SETUP_HEIGHT - SETUP_MARGIN - 3 * SETUP_LINE, text.c_str(), SETUP_GREEN);
        }

        const char* help = typing ? "Enter: use this folder   Ctrl+V: paste   Esc: back" : "Up/Down or mouse: choose   Enter: confirm   Esc: quit";
        setup_text(screen, SETUP_MARGIN, SETUP_HEIGHT - SETUP_MARGIN - SETUP_LINE, help, SETUP_DIM);

        setup_present(screen);

        if (frame == 0 && screenshot != NULL) {
            setup_save_screenshot(screen, screenshot);
        }

        frame++;
        SDL_Delay(16);
    }

    if (result) {
        // Let "Starting..." show for a moment.
        SDL_Delay(400);
    }

    SDL_StopTextInput();
    SDL_DestroyTexture(screen->texture);
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
    delete screen;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    return result;
}

} // namespace fallout
