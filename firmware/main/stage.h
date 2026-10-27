#pragma once
#include <stdint.h>
#include "face.h"

// The pet's stage: a full-width band of the screen drawn without LVGL, a few
// rows at a time, straight into an RGB565 strip that goes to the panel.
// Eyes, pupils and particles are horizontal spans with 4× vertical
// supersampling and fractional edges, so curves stay smooth without a GPU.
// The ESP32-C6 has no FPU: everything per row is fixed-point integer maths;
// floats only appear once per frame. Pure C++: firmware/test/ renders frames
// to images on the Mac.

static constexpr int STAGE_W = 410;
static constexpr int STAGE_H = 300;
// Design units (240 × 180) to stage pixels.
static constexpr float STAGE_SCALE = 1.95f;
static constexpr float STAGE_OX = STAGE_W / 2.0f - VIEW_W / 2 * STAGE_SCALE;
// Vertical centre of the eyes in stage pixels (room above for the bounce).
static constexpr float STAGE_EYES_Y = 156;
static constexpr float STAGE_OY = STAGE_EYES_Y - EYE_CY * STAGE_SCALE;

struct Dot {
  float x, y, r, a;  // stage px, alpha 0..1
};
struct Heart {
  float x, y, s, a;
};
struct Zed {
  float x, y, size, a;  // baseline-left, like SVG text
};

static constexpr int MAX_DOTS = 20;

struct StageFrame {
  EyeShape eyes[2];
  uint32_t eye = 0, pupil = 0;  // 0xRRGGBB
  Dot dots[MAX_DOTS];
  int nDots = 0;
  Heart hearts[3];
  int nHearts = 0;
  Zed zeds[3];
  int nZeds = 0;
};

// Converts a frame to fixed point; call once per frame before renderStage.
void prepareStage(const StageFrame &f);
// Renders stage rows [y0, y0 + rows) into `buf` (rows × STAGE_W), RGB565
// big-endian as the panel takes it.
void renderStage(int y0, int rows, uint16_t *buf);
