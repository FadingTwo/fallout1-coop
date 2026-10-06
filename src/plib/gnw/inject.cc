#include "plib/gnw/inject.h"

#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"

namespace fallout {

#define INJECT_QUEUE_CAPACITY 256

typedef enum InjectEventType {
    INJECT_EVENT_MOUSE,
    INJECT_EVENT_KEY,
    INJECT_EVENT_WAIT,
    INJECT_EVENT_CALL,
} InjectEventType;

typedef struct InjectEvent {
    InjectEventType type;
    int x;
    int y;
    // Mouse buttons, key code or frames to wait.
    int value;
    void (*proc)();
} InjectEvent;

static void inject_push(InjectEventType type, int x, int y, int value);

static InjectEvent inject_queue[INJECT_QUEUE_CAPACITY];
static int inject_head = 0;
static int inject_count = 0;

// Mouse buttons held by the last injected mouse event.
static int inject_buttons = 0;

static bool inject_exclusive = false;

static InjectMouseTransform* inject_mouse_transform = NULL;

void inject_set_mouse_transform(InjectMouseTransform* transform)
{
    inject_mouse_transform = transform;
}

void inject_mouse(int x, int y, int buttons)
{
    inject_push(INJECT_EVENT_MOUSE, x, y, buttons);
}

void inject_click(int x, int y)
{
    // Buttons only react to a press after they have seen the mouse over
    // them for a frame.
    inject_mouse(x, y, 0);
    inject_mouse(x, y, 0);
    inject_mouse(x, y, 0);
    inject_mouse(x, y, MOUSE_STATE_LEFT_BUTTON_DOWN);
    inject_mouse(x, y, 0);
}

void inject_key(int keyCode)
{
    inject_push(INJECT_EVENT_KEY, 0, 0, keyCode);
}

void inject_wait(int frames)
{
    inject_push(INJECT_EVENT_WAIT, 0, 0, frames);
}

void inject_call(void (*proc)())
{
    inject_push(INJECT_EVENT_CALL, 0, 0, 0);
    if (inject_count != 0) {
        inject_queue[(inject_head + inject_count - 1) % INJECT_QUEUE_CAPACITY].proc = proc;
    }
}

bool inject_pending()
{
    return inject_count != 0;
}

void inject_set_exclusive(bool exclusive)
{
    inject_exclusive = exclusive;
}

bool inject_is_exclusive()
{
    return inject_exclusive;
}

bool inject_update()
{
    if (inject_count == 0) {
        if (inject_exclusive) {
            mouse_simulate_input(0, 0, inject_buttons);
            return true;
        }
        return false;
    }

    InjectEvent* event = &(inject_queue[inject_head]);
    switch (event->type) {
    case INJECT_EVENT_MOUSE:
        if (true) {
            int x;
            int y;
            mouse_get_position(&x, &y);
            int targetX = event->x;
            int targetY = event->y;
            if (inject_mouse_transform != NULL) {
                inject_mouse_transform(&targetX, &targetY);
            }
            inject_buttons = event->value;
            mouse_simulate_input(targetX - x, targetY - y, inject_buttons);
        }
        break;
    case INJECT_EVENT_KEY:
        GNW_add_input_buffer(event->value);
        // Keep the mouse where the injected events left it.
        mouse_simulate_input(0, 0, inject_buttons);
        break;
    case INJECT_EVENT_WAIT:
        mouse_simulate_input(0, 0, inject_buttons);
        if (--event->value > 0) {
            return true;
        }
        break;
    case INJECT_EVENT_CALL:
        mouse_simulate_input(0, 0, inject_buttons);
        if (event->proc != NULL) {
            event->proc();
        }
        break;
    }

    inject_head = (inject_head + 1) % INJECT_QUEUE_CAPACITY;
    inject_count--;

    return true;
}

static void inject_push(InjectEventType type, int x, int y, int value)
{
    if (inject_count == INJECT_QUEUE_CAPACITY) {
        return;
    }

    InjectEvent* event = &(inject_queue[(inject_head + inject_count) % INJECT_QUEUE_CAPACITY]);
    event->type = type;
    event->x = x;
    event->y = y;
    event->value = value;
    inject_count++;
}

} // namespace fallout
