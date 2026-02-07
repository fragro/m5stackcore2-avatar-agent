#include "display_ui.h"
#include "config.h"
#include <M5Unified.h>
#include <Avatar.h>

using namespace m5avatar;

static Avatar avatar;
static bool avatar_running = false;
static bool keyboard_open = false;

// Keyboard state
static String kb_input;
static const char* kb_rows[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};
static const int KB_ROWS = 3;
static const int KB_Y_START = 120;
static const int KB_KEY_W = 29;
static const int KB_KEY_H = 28;
static const int KB_PADDING = 2;

// Colors
#define COL_KB_BG   0x10A2
#define COL_KB_KEY  0x4208
#define COL_KB_FG   0xFFFF
#define COL_SEND_BG 0x2665

static void draw_keyboard();

// Wake listening animation state
static unsigned long wake_blink_timer = 0;
static const unsigned long WAKE_BLINK_INTERVAL = 4000;  // Blink every 4 seconds
static const unsigned long WAKE_BLINK_DURATION = 300;    // Blink lasts 300ms

void display_init() {
    M5.Display.setRotation(1);

    // Set up a cute color palette
    ColorPalette cp;
    cp.set(COLOR_PRIMARY, TFT_BLACK);         // Eyes
    cp.set(COLOR_BACKGROUND, TFT_WHITE);      // Face background
    cp.set(COLOR_SECONDARY, (uint16_t)0xFD20); // Cheeks (orange-pink)

    avatar.setColorPalette(cp);
    avatar.init(8);  // 8-bit color for better quality
    avatar_running = true;

    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText("");

    Serial.println("[UI] Avatar display initialized");
}

Avatar& display_get_avatar() {
    return avatar;
}

void display_set_expression(Expression expr) {
    avatar.setExpression(expr);
}

void display_set_speech(const char* text) {
    avatar.setSpeechText(text);
}

void display_set_mouth(float ratio) {
    avatar.setMouthOpenRatio(ratio);
}

void display_set_gaze(float vertical, float horizontal) {
    avatar.setRightGaze(vertical, horizontal);
    avatar.setLeftGaze(vertical, horizontal);
}

void display_avatar_stop() {
    if (avatar_running) {
        avatar.stop();
        avatar_running = false;
    }
}

void display_avatar_start() {
    if (!avatar_running) {
        avatar.init(8);
        avatar_running = true;
    }
}

int display_check_buttons() {
    if (M5.BtnA.wasPressed()) return 1;  // Talk
    if (M5.BtnB.wasPressed()) return 2;  // Type
    if (M5.BtnC.wasPressed()) return 3;  // Menu
    return 0;
}

void display_keyboard_open() {
    keyboard_open = true;
    kb_input = "";

    display_avatar_stop();

    M5.Display.fillScreen(COL_KB_BG);

    // Input field
    M5.Display.fillRect(4, 8, SCREEN_WIDTH - 8, 28, TFT_BLACK);
    M5.Display.setTextColor(COL_KB_FG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 14);
    M5.Display.print("_");

    // Instructions
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7BEF);
    M5.Display.setCursor(10, 44);
    M5.Display.print("Tap keys. BtnC=close");

    draw_keyboard();
}

bool display_keyboard_update(String& result) {
    if (!keyboard_open) return false;

    auto t = M5.Touch.getDetail();
    if (!t.wasPressed()) return false;

    int tx = t.x;
    int ty = t.y;

    // Check letter rows
    for (int row = 0; row < KB_ROWS; row++) {
        int row_y = KB_Y_START + row * (KB_KEY_H + KB_PADDING);
        if (ty < row_y || ty > row_y + KB_KEY_H) continue;

        int row_len = strlen(kb_rows[row]);
        int row_x_start = (SCREEN_WIDTH - row_len * (KB_KEY_W + KB_PADDING)) / 2;

        for (int col = 0; col < row_len; col++) {
            int key_x = row_x_start + col * (KB_KEY_W + KB_PADDING);
            if (tx >= key_x && tx < key_x + KB_KEY_W) {
                kb_input += kb_rows[row][col];
                goto refresh_input;
            }
        }
    }

    {
        // Bottom row: [Space] [Bksp] [Send]
        int bottom_y = KB_Y_START + KB_ROWS * (KB_KEY_H + KB_PADDING);
        if (ty >= bottom_y && ty < bottom_y + KB_KEY_H + 4) {
            if (tx < 140) {
                kb_input += ' ';
            } else if (tx < 220) {
                if (kb_input.length() > 0)
                    kb_input.remove(kb_input.length() - 1);
            } else {
                if (kb_input.length() > 0) {
                    result = kb_input;
                    return true;
                }
                return false;
            }
        }
    }

refresh_input:
    // Refresh input display
    M5.Display.fillRect(4, 8, SCREEN_WIDTH - 8, 28, TFT_BLACK);
    M5.Display.setTextColor(COL_KB_FG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 14);
    String disp = kb_input + "_";
    // Truncate display if too long
    if (disp.length() > 24) {
        disp = disp.substring(disp.length() - 24);
    }
    M5.Display.print(disp.c_str());

    return false;
}

bool display_keyboard_is_open() {
    return keyboard_open;
}

void display_keyboard_close() {
    keyboard_open = false;
    display_avatar_start();
}

void display_wake_listening_update() {
    unsigned long now = millis();

    // Periodic blink: briefly switch to Neutral (eyes open) then back to Sleepy
    if (now - wake_blink_timer >= WAKE_BLINK_INTERVAL) {
        avatar.setExpression(Expression::Neutral);
        // Small mouth twitch to show it's alive
        avatar.setMouthOpenRatio(0.1f);
    }

    if (now - wake_blink_timer >= WAKE_BLINK_INTERVAL + WAKE_BLINK_DURATION) {
        avatar.setExpression(Expression::Sleepy);
        avatar.setMouthOpenRatio(0.0f);
        wake_blink_timer = now;
    }
}

static void draw_keyboard() {
    M5.Display.setTextSize(1);

    for (int row = 0; row < KB_ROWS; row++) {
        int row_y = KB_Y_START + row * (KB_KEY_H + KB_PADDING);
        int row_len = strlen(kb_rows[row]);
        int row_x_start = (SCREEN_WIDTH - row_len * (KB_KEY_W + KB_PADDING)) / 2;

        for (int col = 0; col < row_len; col++) {
            int key_x = row_x_start + col * (KB_KEY_W + KB_PADDING);
            M5.Display.fillRoundRect(key_x, row_y, KB_KEY_W, KB_KEY_H, 3, COL_KB_KEY);
            M5.Display.setTextColor(COL_KB_FG);
            M5.Display.setCursor(key_x + KB_KEY_W / 2 - 3, row_y + KB_KEY_H / 2 - 4);
            M5.Display.printf("%c", kb_rows[row][col]);
        }
    }

    int bottom_y = KB_Y_START + KB_ROWS * (KB_KEY_H + KB_PADDING);

    M5.Display.fillRoundRect(10, bottom_y, 120, KB_KEY_H + 4, 3, COL_KB_KEY);
    M5.Display.setTextColor(COL_KB_FG);
    M5.Display.setCursor(50, bottom_y + KB_KEY_H / 2 - 2);
    M5.Display.print("SPACE");

    M5.Display.fillRoundRect(140, bottom_y, 70, KB_KEY_H + 4, 3, COL_KB_KEY);
    M5.Display.setCursor(155, bottom_y + KB_KEY_H / 2 - 2);
    M5.Display.print("BKSP");

    M5.Display.fillRoundRect(220, bottom_y, 90, KB_KEY_H + 4, 3, COL_SEND_BG);
    M5.Display.setCursor(245, bottom_y + KB_KEY_H / 2 - 2);
    M5.Display.print("SEND");
}
