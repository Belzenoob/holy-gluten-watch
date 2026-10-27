#pragma once

// LVGL draws only the stats that appear on a long press, below the stage.
// Nothing LVGL draws may touch the stage rows (STAGE_Y … STAGE_Y + STAGE_H):
// the eyes are pushed to the panel directly, every frame.

static constexpr int SCREEN_W = 410;
static constexpr int SCREEN_H = 502;
static constexpr int STAGE_Y = 100;

void uiBegin();
// Filled blocks out of 5.
void uiSetMeters(int satiety, int vitality);
void uiShowStats(bool show);
// A speaker (or crossed-out speaker) shown for a moment after the mute switch.
void uiShowSound(bool on);
