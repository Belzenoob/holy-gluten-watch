#include "face.h"
#include <math.h>

const Expression EXPRESSIONS[E_COUNT] = {
    /* neutral   */ {0, 0, 0.16f, 0, 1, 1},
    /* content   */ {0, 0, 0, 0.58f, 1, 1.05f},
    /* excited   */ {0, 0, 0, 0.1f, 1.06f, 1.1f},
    /* worried   */ {0.16f, -0.45f, 0.08f, 0, 1, 1.12f},
    /* sad       */ {0.14f, -0.9f, 0.12f, 0, 1.04f, 1.3f},
    /* tired     */ {0.52f, -0.3f, 0.12f, 0, 0.98f, 0.95f},
    /* sleep     */ {1, 0, 0, 0, 1, 1},
    /* tickled   */ {0, 0, 0, 1, 1.02f, 1},
    /* annoyed   */ {0.4f, 1, 0.1f, 0, 0.95f, 0.62f},
    /* surprised */ {0, 0, 0, 0, 1.14f, 0.66f},
    /* eating    */ {0.05f, 0, 0, 0.5f, 1, 1.05f},
    /* grumpy    */ {0.5f, 0.5f, 0.08f, 0, 0.96f, 0.85f},
    /* sly       */ {0, 0.12f, 0.36f, 0, 1, 0.95f},
};

ExprName expressionFor(Mood mood, bool asleep) {
  if (asleep) return E_SLEEP;
  switch (mood) {
    case SATIATED: return E_CONTENT;
    case ACTIVE: return E_EXCITED;
    case PECKISH: return E_WORRIED;
    case HUNGRY: return E_SAD;
    case STARVING: return E_TIRED;
    case DORMANT: return E_SLEEP;
    default: return E_NEUTRAL;
  }
}

// Eye and pupil colours, one hue per state (expressions.ts): fed is gold,
// active mint, calm sky; hunger warms (tangerine), pleads (pink), then
// fades (mauve, ash). Night is indigo with a moon pupil, anger the only red.
static const Palette EYES[] = {
    /* newborn  */ {0xF3EFE7, 0xE5487F},
    /* satiated */ {0xFFD23F, 0xC0391B},
    /* active   */ {0x2EE6B0, 0x0B4D40},
    /* calm     */ {0x6CB4FF, 0x12306B},
    /* peckish  */ {0xFF9136, 0x6B2508},
    /* hungry   */ {0xFF7A93, 0x5C0F24},
    /* starving */ {0xB5949C, 0x2A1A1F},
    /* dormant  */ {0x5B626E, 0x1A1D22},
};
static const Palette NIGHT = {0x8C86FF, 0xF5D45E};
static const Palette CROSS = {0xFF3B30, 0xFFE14A};

uint8_t paletteKey(Mood mood, bool asleep, ExprName expression) {
  return expression == E_ANNOYED || expression == E_GRUMPY ? PAL_CROSS : asleep ? PAL_ASLEEP : (uint8_t)mood;
}

Palette paletteFor(Mood mood, bool asleep, ExprName expression) {
  uint8_t key = paletteKey(mood, asleep, expression);
  return key == PAL_CROSS ? CROSS : key == PAL_ASLEEP ? NIGHT : EYES[key];
}

Liveliness livelinessFor(Mood mood, bool asleep) {
  if (mood == DORMANT) return {0, 0, 0, 0, 5, 0};
  if (asleep) return {11, 0, 0, 0, 4.2f, 0};
  switch (mood) {
    case SATIATED: return {4, 2.4f, 0.7f, 0.4f, 3, 0};
    case ACTIVE: return {2.6f, 1.5f, 1, 0.25f, 2.6f, 3.4f};
    case CALM: return {6.5f, 2.8f, 0.8f, 0.3f, 3.4f, 0};
    case PECKISH: return {9, 2.4f, 0.6f, 0.55f, 3.4f, 0};
    case HUNGRY: return {0, 2, 0.5f, 0.75f, 3.8f, 0};
    case STARVING: return {0, 3.6f, 0.35f, 0.6f, 4.4f, 0};
    default: return {7, 2, 0.9f, 0.5f, 3.2f, 0};
  }
}

// ─── Geometry (geometry.ts) ─────────────────────────────────────────────────

const EyeBuild MADRE_EYES = {47, EYE_CY, 44, 52, 0.94f, 0.36f, 1};  // leftScale: a touch of asymmetry
static constexpr float MARGIN = 2;                                   // cuts start outside an open eye

static inline const EyeBuild &buildOf(const EyeInput &e) {
  return e.build ? *e.build : MADRE_EYES;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

Ellipse eyeEllipse(const EyeInput &e) {
  const EyeBuild &k = buildOf(e);
  float s = e.eyeScale * (e.side < 0 ? k.leftScale : 1);
  return {VIEW_W / 2 + e.side * k.dx, k.cy + e.dy, k.rx * s * (1 + (1 - e.stretch) * 0.6f), k.ry * s * e.stretch};
}

// A round pupil, or a slit when pupilAspect is below 1 (rx = r * aspect).
Ellipse pupilCircle(const EyeInput &e) {
  const EyeBuild &k = buildOf(e);
  Ellipse o = eyeEllipse(e);
  float r = o.rx * k.pupilRatio * e.pupil;
  float rx = r * k.pupilAspect;
  float lx = e.lookX, ly = e.lookY;
  float m = sqrtf(lx * lx + ly * ly);
  if (m > 1) {
    lx /= m;
    ly /= m;
  }
  return {o.cx + lx * (o.rx - rx - 6), o.cy + ly * (o.ry - r - 6), rx, r};
}

float lidAmount(const EyeInput &e) {
  return clampf(fmaxf(fmaxf(e.lid, e.blink), fabsf(e.tilt) * 0.3f), 0, 1);
}

Cut upperCut(const EyeInput &e) {
  Ellipse o = eyeEllipse(e);
  float edge = o.cy - o.ry - MARGIN + lidAmount(e) * (2 * o.ry + 2 * MARGIN);
  float slope = e.tilt * o.ry * 0.5f * (1 - clampf(fmaxf(e.lid, e.blink), 0, 1));
  float inner = edge + slope, outer = edge - slope;
  // The left eye's inner corner is on its right.
  return {o.cx - o.rx, e.side < 0 ? outer : inner, o.cx + o.rx, e.side < 0 ? inner : outer};
}

Cut lowerCut(const EyeInput &e) {
  Ellipse o = eyeEllipse(e);
  float edge = o.cy + o.ry + MARGIN - clampf(e.low, 0, 1) * (2 * o.ry + 2 * MARGIN);
  float slope = -e.tilt * o.ry * 0.3f * clampf(e.low * 4, 0, 1);
  float inner = edge + slope, outer = edge - slope;
  return {o.cx - o.rx, e.side < 0 ? outer : inner, o.cx + o.rx, e.side < 0 ? inner : outer};
}

Ellipse happyEllipse(const EyeInput &e) {
  Ellipse o = eyeEllipse(e);
  float rx = o.rx * 1.25f, ry = o.ry * 0.85f;
  float restY = o.cy + o.ry + ry + 2;
  float fullY = o.cy + o.ry * 0.62f;
  return {o.cx, restY - clampf(e.happy, 0, 1) * (restY - fullY), rx, ry};
}

ClosedLine closedLine(const EyeInput &e) {
  Ellipse o = eyeEllipse(e);
  return {o.cx, o.rx * 0.8f, o.cy + o.ry * 0.15f, o.ry * 0.32f, clampf((lidAmount(e) - 0.82f) / 0.18f, 0, 1)};
}

EyeShape eyeShape(const EyeInput &e) {
  EyeShape s;
  s.eye = eyeEllipse(e);
  s.pupil = pupilCircle(e);
  s.happy = happyEllipse(e);
  s.hasHappy = e.happy > 0.001f;
  s.upper = upperCut(e);
  s.lower = lowerCut(e);
  s.line = closedLine(e);
  return s;
}

// Half-width of an ellipse at height y, or -1 outside it.
static float halfWidth(const Ellipse &o, float y) {
  float d = (y - o.cy) / o.ry;
  if (d <= -1 || d >= 1) return -1;
  return o.rx * sqrtf(1 - d * d);
}

void visibleSpans(const EyeShape &s, const Ellipse &shape, float y, Spans &out) {
  out.n = 0;
  float h = halfWidth(shape, y);
  if (h < 0) return;
  float l = shape.cx - h, r = shape.cx + h;

  // Upper cut: keep what lies below the line, y >= y0 + m (x - x0).
  const Cut &u = s.upper;
  float mu = (u.y1 - u.y0) / (u.x1 - u.x0);
  if (fabsf(mu) < 1e-6f) {
    if (y < u.y0) return;
  } else if (mu > 0) {
    r = fminf(r, u.x0 + (y - u.y0) / mu);
  } else {
    l = fmaxf(l, u.x0 + (y - u.y0) / mu);
  }
  // Lower cut: keep what lies above the line.
  const Cut &w = s.lower;
  float ml = (w.y1 - w.y0) / (w.x1 - w.x0);
  if (fabsf(ml) < 1e-6f) {
    if (y > w.y0) return;
  } else if (ml > 0) {
    l = fmaxf(l, w.x0 + (y - w.y0) / ml);
  } else {
    r = fminf(r, w.x0 + (y - w.y0) / ml);
  }
  if (l >= r) return;

  // Happy lid: a bite out of the span.
  float hh = s.hasHappy ? halfWidth(s.happy, y) : -1;
  if (hh < 0) {
    out.a[0] = l;
    out.b[0] = r;
    out.n = 1;
    return;
  }
  float hl = s.happy.cx - hh, hr = s.happy.cx + hh;
  if (fminf(r, hl) > l) {
    out.a[out.n] = l;
    out.b[out.n] = fminf(r, hl);
    out.n++;
  }
  if (r > fmaxf(l, hr)) {
    out.a[out.n] = fmaxf(l, hr);
    out.b[out.n] = r;
    out.n++;
  }
}
