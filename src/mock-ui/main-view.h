#ifndef IBLUECONNECT_MOCK_MAIN_VIEW_H
#define IBLUECONNECT_MOCK_MAIN_VIEW_H

#include <adwaita.h>
#include "mock-state.h"

typedef enum {
    MOCK_MAIN_CALL,
    MOCK_MAIN_CONNECT,
    MOCK_MAIN_DISCONNECT,
    MOCK_MAIN_RETRY,
    MOCK_MAIN_SYNC,
    MOCK_MAIN_QUIT,
    MOCK_MAIN_FORGET,
    MOCK_MAIN_RESUME,
    MOCK_MAIN_SIRI,
    MOCK_MAIN_APPEARANCE /* value is "system", "light" or "dark". */
} MockMainAction;

/* value is the dialed number for MOCK_MAIN_CALL with contact_index == -1. */
typedef void (*MockMainCallback)(MockMainAction action, int contact_index,
                                 const char *value, gpointer user_data);

GtkWidget *mock_main_window_new(GtkApplication *app, MockUiModel *model,
                                MockMainCallback callback, gpointer user_data);
void mock_main_window_update(GtkWidget *window);
/* Shows the Keypad tab, e.g. for Add call from the call controller. */
void mock_main_window_show_dial(GtkWidget *window);

#endif
