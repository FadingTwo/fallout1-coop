#include "game/coop_update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <SDL.h>

#include "game/amutex.h"
#include "game/gconfig.h"
#include "plib/gnw/debug.h"

namespace fallout {

namespace fs = std::filesystem;

// Replaced files get this suffix until the next start removes them.
#define UPDATE_OLD_SUFFIX ".old"
#define UPDATE_WORK_DIR ".update"

// The replaced files, for coop_update_cleanup() to remove.
#define UPDATE_OLD_LIST ".update-old.txt"

static std::vector<std::string> update_args;

void coop_update_init(int argc, char** argv)
{
    update_args.assign(argv, argv + argc);
}

static fs::path update_game_dir()
{
    fs::path dir = fs::current_path();
    char* basePath = SDL_GetBasePath();
    if (basePath != NULL) {
        dir = basePath;
        SDL_free(basePath);
    }
    return dir;
}

void coop_update_cleanup()
{
    fs::path dir = update_game_dir();
    std::error_code error;

    fs::remove_all(dir / UPDATE_WORK_DIR, error);

    FILE* stream = fopen((dir / UPDATE_OLD_LIST).string().c_str(), "rt");
    if (stream == NULL) {
        return;
    }

    std::vector<std::string> remaining;
    char line[1024];
    while (fgets(line, sizeof(line), stream) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        fs::path path = dir / fs::path(line);
        fs::remove(path, error);
        if (error && fs::exists(path)) {
            // On Windows the previous executable may still be closing; it
            // goes next time.
            remaining.push_back(line);
        }
    }
    fclose(stream);

    if (remaining.empty()) {
        fs::remove(dir / UPDATE_OLD_LIST, error);
    } else {
        stream = fopen((dir / UPDATE_OLD_LIST).string().c_str(), "wt");
        if (stream != NULL) {
            for (const std::string& path : remaining) {
                fprintf(stream, "%s\n", path.c_str());
            }
            fclose(stream);
        }
    }
}

// -----------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

static const uint32_t update_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t update_rotr(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

static void update_sha256_block(uint32_t state[8], const unsigned char* block)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = update_rotr(w[i - 15], 7) ^ update_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = update_rotr(w[i - 2], 17) ^ update_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = update_rotr(e, 6) ^ update_rotr(e, 11) ^ update_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + update_k[i] + w[i];
        uint32_t s0 = update_rotr(a, 2) ^ update_rotr(a, 13) ^ update_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

std::string coop_update_sha256(const std::string& data)
{
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    size_t full = data.size() / 64 * 64;
    for (size_t offset = 0; offset < full; offset += 64) {
        update_sha256_block(state, (const unsigned char*)data.data() + offset);
    }

    unsigned char tail[128];
    size_t rest = data.size() - full;
    memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    size_t tailLength = rest + 9 <= 64 ? 64 : 128;
    memset(tail + rest + 1, 0, tailLength - rest - 1);
    uint64_t bits = (uint64_t)data.size() * 8;
    for (int i = 0; i < 8; i++) {
        tail[tailLength - 1 - i] = (unsigned char)(bits >> (i * 8));
    }
    for (size_t offset = 0; offset < tailLength; offset += 64) {
        update_sha256_block(state, tail + offset);
    }

    char hex[65];
    for (int i = 0; i < 8; i++) {
        snprintf(hex + i * 8, 9, "%08x", state[i]);
    }
    return hex;
}

// -----------------------------------------------------------------------------
// Installing

// Runs `tar -xf package -C dir` (bsdtar on Windows 10+ and macOS also
// reads .zip).
static bool update_extract(const fs::path& package, const fs::path& dir, std::string* error)
{
#ifdef _WIN32
    // Windows 10 and later have tar.exe in System32; use that one, not
    // whatever tar.exe the search path finds first.
    std::wstring tar = L"tar.exe";
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        tar = L"\"" + std::wstring(system, length) + L"\\tar.exe\"";
    }

    std::wstring command = tar + L" -xf \"" + package.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    if (!CreateProcessW(NULL, buffer.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        *error = "Could not run tar.exe to unpack.";
        return false;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (exitCode != 0) {
        *error = "Could not unpack the update.";
        return false;
    }
#else
    std::string command = "tar -xf '" + package.string() + "' -C '" + dir.string() + "'";
    if (package.string().find('\'') != std::string::npos || dir.string().find('\'') != std::string::npos || system(command.c_str()) != 0) {
        *error = "Could not unpack the update.";
        return false;
    }
#endif
    return true;
}

bool coop_update_install(const std::string& packagePath, std::string* error)
{
    fs::path dir = update_game_dir();
    fs::path work = dir / UPDATE_WORK_DIR;
    std::error_code fsError;

    fs::remove_all(work, fsError);
    if (!fs::create_directories(work, fsError)) {
        *error = "Could not prepare the update.";
        return false;
    }

    if (!update_extract(packagePath, work, error)) {
        fs::remove_all(work, fsError);
        return false;
    }

    // Packages hold one top folder (fallout-coop-<version>-<system>).
    fs::path source = work;
    int entries = 0;
    fs::path only;
    for (fs::directory_iterator it(work, fsError); !fsError && it != fs::directory_iterator(); it.increment(fsError)) {
        entries++;
        only = it->path();
    }
    if (entries == 1 && fs::is_directory(only, fsError)) {
        source = only;
    }

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(source, fsError); !fsError && it != fs::recursive_directory_iterator(); it.increment(fsError)) {
        if (it->is_regular_file(fsError)) {
            files.push_back(fs::relative(it->path(), source, fsError));
        }
    }

    if (files.empty()) {
        *error = "The update package is empty.";
        fs::remove_all(work, fsError);
        return false;
    }

    FILE* oldList = fopen((dir / UPDATE_OLD_LIST).string().c_str(), "at");

    for (const fs::path& relative : files) {
        fs::path from = source / relative;
        fs::path to = dir / relative;
        fs::create_directories(to.parent_path(), fsError);

        if (fs::exists(to, fsError)) {
            fs::path old = to;
            old += UPDATE_OLD_SUFFIX;
            fs::remove(old, fsError);
            fs::rename(to, old, fsError);
            if (fsError) {
                *error = "Could not replace " + relative.string() + ".";
                if (oldList != NULL) {
                    fclose(oldList);
                }
                fs::remove_all(work, fsError);
                return false;
            }

            if (oldList != NULL) {
                fprintf(oldList, "%s%s\n", relative.generic_string().c_str(), UPDATE_OLD_SUFFIX);
            }
        }

        fs::rename(from, to, fsError);
        if (fsError) {
            fsError.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, fsError);
            if (fsError) {
                *error = "Could not install " + relative.string() + ".";
                if (oldList != NULL) {
                    fclose(oldList);
                }
                fs::remove_all(work, fsError);
                return false;
            }
        }

        debug_printf("\nCOOP UPDATE: installed %s\n", relative.string().c_str());
    }

    if (oldList != NULL) {
        fclose(oldList);
    }

    fs::remove_all(work, fsError);
    fs::remove(packagePath, fsError);
    return true;
}

[[noreturn]] void coop_update_restart()
{
    gconfig_save();
    autorun_mutex_destroy();

#ifdef _WIN32
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    std::vector<wchar_t> commandLine(GetCommandLineW(), GetCommandLineW() + wcslen(GetCommandLineW()) + 1);
    if (CreateProcessW(NULL, commandLine.data(), NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    exit(0);
#else
    // The new executable has the old one's name; argv[0] may be relative to
    // the folder it was started from, which has not changed.
    std::vector<char*> argv;
    for (std::string& arg : update_args) {
        argv.push_back(&arg[0]);
    }
    argv.push_back(NULL);

    if (!update_args.empty()) {
        execv(argv[0], argv.data());
        execvp(argv[0], argv.data());
    }
    exit(0);
#endif
}

} // namespace fallout
