#include "fps_limiter.h"

#include <SDL.h>

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _ticks(0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
}

void FpsLimiter::throttle() const
{
    // CE: the time is read once. Reading it twice, a pause of more than a
    // frame between the reads (a busy machine) made the subtraction wrap
    // around, and the game slept for weeks: it froze.
    unsigned int frameTime = 1000 / _fps;
    unsigned int elapsed = SDL_GetTicks() - _ticks;
    if (elapsed < frameTime) {
        SDL_Delay(frameTime - elapsed);
    }
}

} // namespace fallout
