#include "game/coop_crash.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

#include "game/coop.h"
#include "plib/gnw/debug.h"

namespace fallout {

// Paths are kept ready: the handler must not allocate.
static char coop_crash_path[1024];
static char coop_crash_old_path[1024];

// The handler writes with as little as possible (no allocation, no
// stdio): small helpers for text and hex numbers.
#ifdef _WIN32
typedef HANDLE CrashFile;
#else
typedef int CrashFile;
#endif

static void crash_write(CrashFile file, const char* data, size_t length)
{
#ifdef _WIN32
    DWORD written;
    WriteFile(file, data, (DWORD)length, &written, NULL);
#else
    while (length > 0) {
        ssize_t count = write(file, data, length);
        if (count <= 0) {
            return;
        }
        data += count;
        length -= (size_t)count;
    }
#endif
}

static void crash_text(CrashFile file, const char* text)
{
    crash_write(file, text, strlen(text));
}

static void crash_hex(CrashFile file, unsigned long long value)
{
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int index = 0; index < 16; index++) {
        buffer[2 + index] = "0123456789abcdef"[(value >> (60 - 4 * index)) & 0xF];
    }
    buffer[18] = '\0';
    crash_text(file, buffer);
}

static void crash_write_log(CrashFile file)
{
    const char* first;
    size_t firstLength;
    const char* second;
    size_t secondLength;
    debug_get_recent_parts(&first, &firstLength, &second, &secondLength);
    crash_text(file, "\n--- Recent log ---\n");
    crash_write(file, first, firstLength);
    crash_write(file, second, secondLength);
    crash_text(file, "\n");
}

static void crash_write_header(CrashFile file, const char* what)
{
    crash_text(file, "Fallout CE co-op crash report\nVersion: " COOP_VERSION "\nCrash: ");
    crash_text(file, what);
    crash_text(file, "\n");
}

#ifdef _WIN32

static LONG WINAPI coop_crash_handler(EXCEPTION_POINTERS* info)
{
    HANDLE file = CreateFileA(coop_crash_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        crash_write_header(file, "Windows exception");
        crash_text(file, "Code: ");
        crash_hex(file, info->ExceptionRecord->ExceptionCode);
        crash_text(file, "\nAddress: ");
        crash_hex(file, (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionAddress);
        crash_text(file, "\nModule base: ");
        crash_hex(file, (unsigned long long)(uintptr_t)GetModuleHandleA(NULL));
        crash_text(file, "\nPlatform: Windows\n");
        crash_write_log(file);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void coop_crash_install(const char* path, bool catchCrashes)
{
    snprintf(coop_crash_path, sizeof(coop_crash_path), "%s", path);
    if (catchCrashes) {
        SetUnhandledExceptionFilter(coop_crash_handler);
    }
}

#else

static unsigned long long coop_crash_base = 0;

static int coop_crash_find_base(struct dl_phdr_info* info, size_t size, void* data)
{
    // The first object is the program itself.
    coop_crash_base = (unsigned long long)info->dlpi_addr;
    return 1;
}

static void coop_crash_handler(int signal, siginfo_t* info, void* context)
{
    int file = open(coop_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file != -1) {
        const char* what = signal == SIGSEGV ? "SIGSEGV (bad memory access)"
            : signal == SIGBUS             ? "SIGBUS"
            : signal == SIGFPE             ? "SIGFPE"
            : signal == SIGILL             ? "SIGILL"
            : signal == SIGABRT            ? "SIGABRT (abort)"
                                           : "signal";
        crash_write_header(file, what);
        crash_text(file, "Fault address: ");
        crash_hex(file, (unsigned long long)(uintptr_t)info->si_addr);
        crash_text(file, "\nProgram base: ");
        crash_hex(file, coop_crash_base);
        crash_text(file, "\nPlatform: Linux\nBacktrace:\n");
#if defined(__GLIBC__)
        void* frames[48];
        int count = backtrace(frames, 48);
        for (int index = 0; index < count; index++) {
            crash_text(file, "  ");
            crash_hex(file, (unsigned long long)(uintptr_t)frames[index]);
            crash_text(file, "\n");
        }
#endif
        crash_write_log(file);
        close(file);
    }

    // On to the default action (the handler was reset).
    raise(signal);
}

void coop_crash_install(const char* path, bool catchCrashes)
{
    snprintf(coop_crash_path, sizeof(coop_crash_path), "%s", path);
    if (!catchCrashes) {
        return;
    }
    dl_iterate_phdr(coop_crash_find_base, NULL);

#if defined(__GLIBC__)
    // backtrace() loads libgcc on first use: not in the handler.
    void* warmUp[1];
    backtrace(warmUp, 1);
#endif

    // Its own stack, so a stack overflow can be reported too.
    static char alternateStack[64 * 1024];
    stack_t stack;
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = coop_crash_handler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    const int signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (int signal : signals) {
        sigaction(signal, &action, NULL);
    }
}

#endif

bool coop_crash_pending(std::string* report)
{
    if (coop_crash_path[0] == '\0') {
        return false;
    }

    FILE* stream = fopen(coop_crash_path, "rb");
    if (stream == NULL) {
        return false;
    }

    report->clear();
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), stream)) > 0 && report->size() < 256 * 1024) {
        report->append(buffer, count);
    }
    fclose(stream);
    return !report->empty();
}

void coop_crash_done()
{
    if (coop_crash_path[0] == '\0') {
        return;
    }
    snprintf(coop_crash_old_path, sizeof(coop_crash_old_path), "%s", coop_crash_path);
    char* dot = strrchr(coop_crash_old_path, '.');
    if (dot != NULL) {
        snprintf(dot, sizeof(coop_crash_old_path) - (size_t)(dot - coop_crash_old_path), "-old.txt");
    }
    remove(coop_crash_old_path);
    rename(coop_crash_path, coop_crash_old_path);
}

void coop_crash_now()
{
    volatile int* nowhere = NULL;
    *nowhere = 42;
}

} // namespace fallout
