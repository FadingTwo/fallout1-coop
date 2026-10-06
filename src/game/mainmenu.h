#ifndef FALLOUT_GAME_MAINMENU_H_
#define FALLOUT_GAME_MAINMENU_H_

namespace fallout {

typedef enum MainMenuOption {
    MAIN_MENU_INTRO,
    MAIN_MENU_NEW_GAME,
    MAIN_MENU_LOAD_GAME,
    MAIN_MENU_SCREENSAVER,
    MAIN_MENU_TIMEOUT,
    MAIN_MENU_CREDITS,
    MAIN_MENU_QUOTES,
    MAIN_MENU_EXIT,
    MAIN_MENU_SELFRUN,
    MAIN_MENU_OPTIONS,
    // CE: co-op.
    MAIN_MENU_MULTIPLAYER,
} MainMenuOption;

extern bool in_main_menu;

int main_menu_create();
void main_menu_destroy();
void main_menu_hide(bool animate);
void main_menu_show(bool animate);
int main_menu_is_shown();
int main_menu_is_enabled();
void main_menu_set_timeout(unsigned int timeout);
unsigned int main_menu_get_timeout();
int main_menu_loop();

// CE: Sub-menus in the main menu's button panel (co-op). Shows `count`
// (at most 6) buttons labelled `labels`, with hot keys `keys`, and returns
// the index picked, or -1 for Escape. The main menu's own buttons come back
// with main_menu_restore().
int main_menu_choose(const char* const* labels, const int* keys, int count);

// CE: Text entry in the panel, with `label` as its title. Returns false if
// cancelled.
bool main_menu_input(const char* label, const char* hint, char* text, int maxLength);

// CE: A line of text in the panel's bottom slot ("" clears it). Only for
// button sets that leave that slot free.
void main_menu_status(const char* text);

// CE: Shows a button set without waiting for a choice.
void main_menu_show_buttons(const char* const* labels, const int* keys, int count);

void main_menu_restore();

} // namespace fallout

#endif /* FALLOUT_GAME_MAINMENU_H_ */
