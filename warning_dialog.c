#include "warning_dialog.h"
#include <raylib.h>

typedef struct
{
    bool active;
    char msg[512];
    WarningType type;
    char title[128];
} WarningDialog;

static WarningDialog warning_dialog = {0};

void show_warning(const char *title, const char *msg, WarningType type)
{
    warning_dialog.active = true;
    strncpy(warning_dialog.title, title, 127);
    warning_dialog.title[127] = '\0';

    strncpy(warning_dialog.msg, msg, 511);
    warning_dialog.msg[511] = '\0';
    warning_dialog.type = type;
}

void draw_warning_dialog(void)
{
    if (warning_dialog.active == false) return;
    int text_width = MeasureText(warning_dialog.msg, 16);
    int lines = (text_width / 400) + 1;

    int window_width = 450;
    int window_height = 120 + lines * 20;

    Rectangle window_bounds = {
        (WINDOW_WIDTH - window_width) / 2,
        (WINDOW_HEIGHT - window_height) / 2,
        window_width,
        window_height
    };

    char *icon;
    Color titleColor = BLACK;

    switch (warning_dialog.type)
    {
    case WARNING_TYPE_INFO:
        icon = "#142#";
        titleColor = BLUE;
        break;
    case WARNING_TYPE_WARNING:
        icon = "#78#";
        titleColor = ORANGE;
        break;
    case WARNING_TYPE_ERROR:
        icon = "#159#";
        titleColor = RED;
        break;
    case WARNING_TYPE_SUCCESS:
        icon = "#84#";
        titleColor = GREEN;
        break;
    default:
        icon = "#142#";
        titleColor = GRAY;
    }

    char formatted_title[256];
    snprintf(formatted_title, sizeof(formatted_title), "%s%s", icon, warning_dialog.title);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(titleColor));

    int result = GuiMessageBox(window_bounds, formatted_title, warning_dialog.msg, "Ok");

    // Reset color
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(BLACK));
    if (result >= 0)
    {
        warning_dialog.active = false;
        TraceLog(LOG_WARNING, "warning_dialog.active=%s", warning_dialog.active == true ? "true" : "false");
    }
}

void show_error(const char *msg)
{
    show_warning("ERROR", msg, WARNING_TYPE_ERROR);
}

void show_info(const char *msg)
{
    show_warning("INFO", msg, WARNING_TYPE_INFO);
}

void show_success(const char *msg)
{
    show_warning("SUCCESS", msg, WARNING_TYPE_SUCCESS);
}

void show_warning_msg(const char *msg)
{
    show_warning("WARNING", msg, WARNING_TYPE_WARNING);
}
