#pragma once

#include <Arduino.h>
#include <Avatar.h>

/// Initialize display and start avatar face.
void display_init();

/// Get avatar reference for direct manipulation.
m5avatar::Avatar& display_get_avatar();

/// Set expression on the avatar face.
void display_set_expression(m5avatar::Expression expr);

/// Show speech text balloon.
void display_set_speech(const char* text);

/// Set mouth open ratio for lip sync (0.0 - 1.0).
void display_set_mouth(float ratio);

/// Set avatar gaze direction (-1.0 to 1.0 for both axes).
void display_set_gaze(float vertical, float horizontal);

/// Stop avatar rendering (e.g. for keyboard mode).
void display_avatar_stop();

/// Resume avatar rendering.
void display_avatar_start();

/// Check Core2 touch buttons.
/// Returns: 0=none, 1=BtnA(Talk), 2=BtnB(Type), 3=BtnC(Menu)
int display_check_buttons();

/// Open on-screen keyboard. Stops avatar.
void display_keyboard_open();

/// Update keyboard. Returns true when user submits text.
bool display_keyboard_update(String& result);

/// Returns true if keyboard is open.
bool display_keyboard_is_open();

/// Close keyboard and restart avatar.
void display_keyboard_close();
