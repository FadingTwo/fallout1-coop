#ifndef FALLOUT_PLIB_GNW_DEBUG_H_
#define FALLOUT_PLIB_GNW_DEBUG_H_

#include <stddef.h>

namespace fallout {

typedef int(DebugFunc)(char* string);

void GNW_debug_init();
void debug_register_mono();
void debug_register_log(const char* fileName, const char* mode);
void debug_register_screen();
void debug_register_env();
void debug_register_func(DebugFunc* func);
int debug_printf(const char* format, ...);

// CE: Copies the most recent debug output (up to `size` - 1 characters)
// into `buffer`; returns its length.
size_t debug_get_recent(char* buffer, size_t size);

// The same as two pieces in place (oldest first), without copying: safe
// to use from a crash handler.
void debug_get_recent_parts(const char** first, size_t* firstLength, const char** second, size_t* secondLength);
int debug_puts(char* string);
void debug_clear();

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_DEBUG_H_ */
