#pragma once
#include <stdint.h>
#include "companion.h"
#include "face.h"

// The pet's stage: a full-width band of the screen drawn without LVGL, a few
// rows at a time, straight into an RGB565 strip that goes to the panel.
// Eyes, pupils, particles and a companion's parts are horizontal spans with 4×
// vertical supersampling and fractional edges, so curves stay smooth without a GPU.
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

// A companion's drawn parts this frame (companion.h): the pack's shapes for the
// state the pet is morphing from and to, moved by the inputs. Each part is its
// own layer, composited in order: back parts, the eyes, front parts.
struct CompDraw {
  const Companion *c = nullptr;  // nullptr: Madre, eyes only
  int stateA = 0, stateB = 0;    // companionState() morphed from and to
  float mix = 1;                 // 0 = A … 1 = B
  float gazeX = 0, gazeY = 0;    // −1..1
  float breath = 0.5f;           // 0..1: the pack's rest is 0.5
  float bounce = 0;              // 0 at rest, up to 1 (negative while it settles)
  float dy = 0;                  // the whole figure pressed down by this much, design units
  uint32_t accent = 0, accentSoft = 0;  // 0xRRGGBB
};

struct StageFrame {
  EyeShape eyes[2];
  uint32_t eye = 0, pupil = 0;  // 0xRRGGBB
  Dot dots[MAX_DOTS];
  int nDots = 0;
  Heart hearts[3];
  int nHearts = 0;
  Zed zeds[3];
  int nZeds = 0;
  CompDraw comp;
};

// Converts a frame to fixed point; call once per frame before renderStage.
void prepareStage(const StageFrame &f);
// Renders stage rows [y0, y0 + rows) into `buf` (rows × STAGE_W), RGB565
// big-endian as the panel takes it.
void renderStage(int y0, int rows, uint16_t *buf);

#ifdef STAGE_STATS
// Work counted while rendering, for the Mac's cost model (firmware/test).
struct StageStats {
  long edgeTests, crossings, sqrts, divs, spans, pixelsBlended, pixelsWalked, pixelsFilled, pixelsFilled32, composed, pointOps;
};
extern StageStats stageStats;
#endif
