#ifndef WARNING_DIALOG_H
#define WARNING_DIALOG_H

#include <string.h>
#include "window.h"
#include <stdio.h>
#include <raylib.h>
#include "raygui.h"
#include <stdbool.h>

// Only declare types and function prototypes here
typedef enum {
    WARNING_TYPE_INFO,
    WARNING_TYPE_WARNING,
    WARNING_TYPE_ERROR,
    WARNING_TYPE_SUCCESS
} WarningType;

// Function declarations only
void show_warning(const char *title, const char *msg, WarningType type);
void draw_warning_dialog(void);
void show_error(const char *message);
void show_info(const char *message);
void show_success(const char *message);
void show_warning_msg(const char *message);

#endif // WARNING_DIALOG_H
