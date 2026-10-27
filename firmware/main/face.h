#pragma once
#include <stdint.h>
#include "brain.h"

// Expressions, colours and eye geometry: a port of expressions.ts and
// geometry.ts (src/features/starter/pet/). Same numbers, same 240 × 180
// design space; the eyes are drawn as horizontal spans instead of polygons.

enum ExprName : uint8_t {
  E_NEUTRAL, E_CONTENT, E_EXCITED, E_WORRIED, E_SAD, E_TIRED, E_SLEEP,
  E_TICKLED, E_ANNOYED, E_SURPRISED, E_EATING, E_GRUMPY, E_SLY, E_COUNT
};

struct Expression {
  float lid, tilt, low, happy, eyeScale, pupil;
};

extern const Expression EXPRESSIONS[E_COUNT];
ExprName expressionFor(Mood mood, bool asleep);

struct Palette {
  uint32_t eye, pupil;
};
Palette paletteFor(Mood mood, bool asleep, ExprName expression);

struct Liveliness {
  float bubbleSeconds;  // 0 = no bubbles
  float glanceSeconds;  // 0 = still
  float glanceRange;
  float lookAtYou;
  float breathSeconds;
  float hopSeconds;  // little hops on the spot; 0 = none
};
Liveliness livelinessFor(Mood mood, bool asleep);

// ─── Geometry ────────────────────────────────────────────────────────────────

static constexpr float VIEW_W = 240;
static constexpr float VIEW_H = 180;
static constexpr float EYE_CY = 92;

struct EyeInput {
  int side;  // -1 left, +1 right
  float lid, tilt, low, happy, eyeScale, pupil;
  float lookX, lookY;  // -1..1
  float blink;         // 0..1
  float dy;            // breathing / bounce offset
  float stretch;       // squash & stretch
};

struct Ellipse {
  float cx, cy, rx, ry;
};

// A cut line from (x0, y0) to (x1, y1), left to right.
struct Cut {
  float x0, y0, x1, y1;
};

// The relaxed closed eye ‿: y(x) = y + 2·bend·t(1−t), t = (x − (cx − w)) / 2w.
struct ClosedLine {
  float cx, w, y, bend, opacity;
};

Ellipse eyeEllipse(const EyeInput &e);
Ellipse pupilCircle(const EyeInput &e);
float lidAmount(const EyeInput &e);
Cut upperCut(const EyeInput &e);
Cut lowerCut(const EyeInput &e);
Ellipse happyEllipse(const EyeInput &e);
ClosedLine closedLine(const EyeInput &e);

// Everything needed to draw one eye, computed once per frame.
struct EyeShape {
  Ellipse eye, pupil, happy;
  bool hasHappy;
  Cut upper, lower;
  ClosedLine line;
};
EyeShape eyeShape(const EyeInput &e);

// Up to two horizontal spans [a, b) at design height y.
struct Spans {
  float a[2], b[2];
  int n;
};
// What the lids leave of `shape` at height y: between the cuts, minus the happy lid.
void visibleSpans(const EyeShape &s, const Ellipse &shape, float y, Spans &out);
