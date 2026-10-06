#ifndef FALLOUT_PLIB_GNW_INJECT_H_
#define FALLOUT_PLIB_GNW_INJECT_H_

namespace fallout {

// Synthetic input, consumed one event per frame by the input loop
// (process_bk) in place of the real mouse while events are queued. Used by
// automated tests; with an empty queue input works as usual.

// Moves the mouse to the absolute screen position (x, y) with `buttons`
// (MOUSE_STATE_*) held.
void inject_mouse(int x, int y, int buttons);

// Left click at (x, y): press, then release on the next frame.
void inject_click(int x, int y);

// A key press, as a game key code (KEY_*).
void inject_key(int keyCode);

// Does nothing for `frames` frames.
void inject_wait(int frames);

// Calls `proc` from the input loop (e.g. to take a screenshot of whatever
// screen is open).
void inject_call(void (*proc)());

bool inject_pending();

// Maps injected mouse positions when they are used (not when queued), e.g.
// to follow a screen layout that is only known later. NULL for none.
typedef void(InjectMouseTransform)(int* x, int* y);
void inject_set_mouse_transform(InjectMouseTransform* transform);

// While exclusive, injected input is the only input: the real mouse is
// ignored even when nothing is queued, and keys pressed locally wait.
void inject_set_exclusive(bool exclusive);
bool inject_is_exclusive();

// Called once per frame by the input loop. Returns true when it drove the
// mouse this frame (the real mouse is then ignored).
bool inject_update();

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_INJECT_H_ */
