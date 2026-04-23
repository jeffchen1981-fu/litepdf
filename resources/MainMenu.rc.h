#pragma once
#define IDM_MAIN_MENU    1

#define IDM_FILE_OPEN    40001
#define IDM_FILE_EXIT    40002
#define IDM_HELP_ABOUT   40003

#define IDM_ZOOM_IN      40010
#define IDM_ZOOM_OUT     40011
#define IDM_ZOOM_RESET   40012

#define IDM_VIEW_OUTLINE 40013

// MRU dynamic separator: inserted between MRU entries and Exit when MRU
// is non-empty. Sits in the gap between IDM_VIEW_OUTLINE (40013) and the
// IDM_MRU_* range (40020+) so it has a stable, command-by-ID handle.
#define IDM_MRU_SEPARATOR 40019

// MRU menu items: contiguous range 40020-40029
#define IDM_MRU_1        40020
#define IDM_MRU_2        40021
#define IDM_MRU_3        40022
#define IDM_MRU_4        40023
#define IDM_MRU_5        40024
#define IDM_MRU_6        40025
#define IDM_MRU_7        40026
#define IDM_MRU_8        40027
#define IDM_MRU_9        40028
#define IDM_MRU_10       40029

// Phase 5: tab management. Added as accelerators only — not in the File menu
// popup, to keep the empty-MRU layout from Phase 4 unchanged.
#define IDM_TAB_CLOSE    40030   // Ctrl+W
#define IDM_TAB_NEXT     40031   // Ctrl+Tab
#define IDM_TAB_PREV     40032   // Ctrl+Shift+Tab
// Ctrl+1..9 jump to tab 1..9 (range 40033-40041, 1-indexed in user UI).
#define IDM_TAB_GOTO_1   40033
#define IDM_TAB_GOTO_2   40034
#define IDM_TAB_GOTO_3   40035
#define IDM_TAB_GOTO_4   40036
#define IDM_TAB_GOTO_5   40037
#define IDM_TAB_GOTO_6   40038
#define IDM_TAB_GOTO_7   40039
#define IDM_TAB_GOTO_8   40040
#define IDM_TAB_GOTO_9   40041

// Phase 6: in-doc and cross-tab search. Keyboard-only, not in any menu popup
// (the floating find bar and docked results panel each have their own UI).
#define IDM_FIND              40042   // Ctrl+F
#define IDM_FIND_NEXT         40043   // F3
#define IDM_FIND_PREV         40044   // Shift+F3
#define IDM_CROSS_TAB_FIND    40045   // Ctrl+Shift+F
#define IDM_TOGGLE_RESULTS    40046   // F6
#define IDM_FIND_CLOSE        40047   // ESC when find bar focused (fallback accel)

// Next free ID: 40048. Reserve 40048-40059 for future search commands
// (e.g., IDM_FIND_ALL_HIGHLIGHT, IDM_FIND_CASE_TOGGLE, IDM_FIND_REGEX).
