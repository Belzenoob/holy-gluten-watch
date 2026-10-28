#include "stage.h"
#include <math.h>
#include <string.h>
#include <utility>

// Fixed point: coordinates in 1/16 px (Q4).
static constexpr int FP = 4;
static constexpr int32_t ONE = 1 << FP;
static constexpr int SUB = 4;  // vertical samples per row
static constexpr int FULL = 256;
static constexpr int W_SUB = FULL / SUB;
static constexpr float LINE_HALF = 3;  // closed line: half stroke, design units
static constexpr int MASK = 32;        // sprite masks are MASK × MASK

#ifdef STAGE_STATS
StageStats stageStats;
#define STAT(field, n) (stageStats.field += (n))
#else
#define STAT(field, n) ((void)0)
#endif

static inline int32_t q(float v) {
  return (int32_t)lroundf(v * ONE);
}

static uint32_t isqrt(uint32_t v) {
  STAT(sqrts, 1);
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

// ─── Fixed-point frame ──────────────────────────────────────────────────────

struct QEll {
  int32_t cx, cy, rx, ry;
  int32_t ratio;  // rx / ry, Q16
  uint32_t ry2;
};

struct QCut {
  bool flat;
  int32_t x0, y0;
  int32_t invm;  // 1 / slope, Q16
  bool rising;   // slope > 0 (y grows with x)
};

struct QEye {
  QEll eye, pupil, happy;
  bool hasHappy;
  QCut upper, lower;
  int32_t top, bottom;  // stage px rows covered by the eye
  // Closed line.
  bool line;
  int32_t lcx, lw, ly, lb, lhw;  // centre, half length, y, bend, half stroke (Q4)
  int lineW;                     // coverage weight per sample
};

struct QDot {
  int32_t cx, cy, r;
  uint32_t r2;
  int w;
};

struct QSprite {
  const uint8_t *mask;
  int32_t x0, y0, w, h;  // box, Q4
  int a;                 // 0..256
};

static bool eyesOn = true;  // false when the companion draws its eyes itself
static QEye eyes[2];
static QDot dots[MAX_DOTS];
static int nDots = 0;
static QSprite sprites[6];
static int nSprites = 0;
static uint16_t eye565, pupil565;      // as the eyes alone are drawn (truncated: Madre's look is fixed)
static uint16_t eye565r, pupil565r;    // with a companion: rounded, so dark colours keep their tint
static int er, eg, eb, pr, pg, pb;

static uint8_t heartMask[MASK * MASK], zedMask[MASK * MASK];
static bool masksReady = false;

static QEll qell(const Ellipse &e) {
  QEll o;
  o.cx = q(STAGE_OX + e.cx * STAGE_SCALE);
  o.cy = q(STAGE_OY + e.cy * STAGE_SCALE);
  o.rx = q(e.rx * STAGE_SCALE);
  o.ry = q(e.ry * STAGE_SCALE);
  if (o.ry < 1) o.ry = 1;
  o.ratio = (int32_t)(((int64_t)o.rx << 16) / o.ry);
  o.ry2 = (uint32_t)o.ry * (uint32_t)o.ry;
  return o;
}

static QCut qcut(const Cut &c) {
  QCut o;
  float m = (c.y1 - c.y0) / (c.x1 - c.x0);
  o.x0 = q(STAGE_OX + c.x0 * STAGE_SCALE);
  o.y0 = q(STAGE_OY + c.y0 * STAGE_SCALE);
  o.flat = fabsf(m) < 1e-4f;
  o.invm = o.flat ? 0 : (int32_t)(65536.0f / m);
  o.rising = m > 0;
  if (o.flat) o.y0 = q(STAGE_OY + (c.y0 + c.y1) / 2 * STAGE_SCALE);
  return o;
}

static inline int32_t cutX(const QCut &c, int32_t y) {
  return c.x0 + (int32_t)(((int64_t)(y - c.y0) * c.invm) >> 16);
}

static inline uint16_t be565(int r, int g, int b) {
  uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((c >> 8) | (c << 8));
}

// The same, rounded to the nearest 565 step instead of truncated.
static inline uint16_t be565r(int r, int g, int b) {
  r = r > 251 ? 255 : r + 4;
  g = g > 253 ? 255 : g + 2;
  b = b > 251 ? 255 : b + 4;
  return be565(r, g, b);
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay;
  float t = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0, 1);
  float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
  return sqrtf(dx * dx + dy * dy);
}

// Sprites are computed once with floats, then only sampled.
static void buildMasks() {
  for (int my = 0; my < MASK; my++)
    for (int mx = 0; mx < MASK; mx++) {
      int heart = 0;
      float zed = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          float u = (mx + (sx + 0.5f) / 4) / MASK, v = (my + (sy + 0.5f) / 4) / MASK;
          // Heart box: 1.1 s square from (x − 0.55 s, y − 0.65 s); classic implicit heart.
          float hx = (u * 1.1f - 0.55f) / 0.43f, hy = 1.24f - (v * 1.1f - 0.05f) / 0.43f;
          float a = hx * hx + hy * hy - 1;
          heart += a * a * a - hx * hx * hy * hy * hy <= 0;
          // Zed box: 0.64 × 0.69 size from (x − 0.07, y − 0.62); three strokes.
          float zx = u * 0.64f - 0.07f, zy = v * 0.69f - 0.62f;
          float d = fminf(fminf(segDist(zx, zy, 0, -0.55f, 0.5f, -0.55f), segDist(zx, zy, 0.5f, -0.55f, 0, 0)), segDist(zx, zy, 0, 0, 0.5f, 0));
          zed += d <= 0.07f;
        }
      heartMask[my * MASK + mx] = (uint8_t)(heart * 255 / 16);
      zedMask[my * MASK + mx] = (uint8_t)(zed * 255 / 16);
    }
  masksReady = true;
}

static void prepareParts(const StageFrame &f);

void prepareStage(const StageFrame &f) {
  if (!masksReady) buildMasks();
  er = (f.eye >> 16) & 0xFF;
  eg = (f.eye >> 8) & 0xFF;
  eb = f.eye & 0xFF;
  pr = (f.pupil >> 16) & 0xFF;
  pg = (f.pupil >> 8) & 0xFF;
  pb = f.pupil & 0xFF;
  eye565 = be565(er, eg, eb);
  pupil565 = be565(pr, pg, pb);
  eye565r = be565r(er, eg, eb);
  pupil565r = be565r(pr, pg, pb);

  for (int i = 0; i < 2; i++) {
    const EyeShape &s = f.eyes[i];
    QEye &e = eyes[i];
    e.eye = qell(s.eye);
    e.pupil = qell(s.pupil);
    e.hasHappy = s.hasHappy;
    e.happy = qell(s.happy);
    e.upper = qcut(s.upper);
    e.lower = qcut(s.lower);
    e.top = (e.eye.cy - e.eye.ry) >> FP;
    e.bottom = ((e.eye.cy + e.eye.ry) >> FP) + 1;
    const ClosedLine &l = s.line;
    e.line = l.opacity > 0.003f;
    e.lcx = q(STAGE_OX + l.cx * STAGE_SCALE);
    e.lw = q(l.w * STAGE_SCALE);
    e.ly = q(STAGE_OY + l.y * STAGE_SCALE);
    e.lb = q(l.bend * STAGE_SCALE);
    e.lhw = q(LINE_HALF * STAGE_SCALE);
    e.lineW = (int)(W_SUB * l.opacity);
  }

  eyesOn = !(f.comp.c && f.comp.c->ownEyes);
  prepareParts(f);

  nDots = 0;
  for (int i = 0; i < f.nDots; i++) {
    const Dot &d = f.dots[i];
    if (d.a <= 0.003f) continue;
    QDot &o = dots[nDots++];
    o.cx = q(d.x);
    o.cy = q(d.y);
    o.r = q(d.r);
    o.r2 = (uint32_t)o.r * (uint32_t)o.r;
    o.w = (int)(W_SUB * d.a);
  }
  nSprites = 0;
  for (int i = 0; i < f.nHearts; i++) {
    const Heart &h = f.hearts[i];
    if (h.a <= 0.003f) continue;
    sprites[nSprites++] = {heartMask, q(h.x - 0.55f * h.s), q(h.y - 0.65f * h.s), q(1.1f * h.s), q(1.1f * h.s), (int)(FULL * h.a)};
  }
  for (int i = 0; i < f.nZeds; i++) {
    const Zed &z = f.zeds[i];
    if (z.a <= 0.003f) continue;
    sprites[nSprites++] = {zedMask, q(z.x - 0.07f * z.size), q(z.y - 0.62f * z.size), q(0.64f * z.size), q(0.69f * z.size), (int)(FULL * z.a)};
  }
}

// ─── Spans ──────────────────────────────────────────────────────────────────
//
// Coverage is kept as edges, not filled pixel by pixel: a span adds its full
// weight to a difference array at its first full pixel and removes it after
// its last; its two fractional end pixels go to an edge array. The composite
// then walks the row once, recomputing the colour only at marked pixels and
// filling the runs in between with one colour.

// PART: particles; EYE, PUPIL, LINE: the eyes. GEN is the one layer a companion's
// part is drawn in, one part at a time.
enum Layer { PART, EYE, PUPIL, LINE, GEN, LAYERS };
static constexpr int EYE_LAYERS = GEN;  // the layers the eyes' single pass composites
static int16_t diff[LAYERS][STAGE_W + 1], edge[LAYERS][STAGE_W + 1];
alignas(4) static uint8_t marked[STAGE_W + 8];  // padded to whole words: the walkers skip four at a time
static int lo, hi;

static inline void mark(int x) {
  marked[x] = 1;
  if (x < lo) lo = x;
  if (x > hi) hi = x;
}

// Adds weight `w` over [xa, xb) (Q4) to a layer, with fractional end pixels.
static void addSpan(int layer, int32_t xa, int32_t xb, int w) {
  STAT(spans, 1);
  if (w <= 0 || xb <= 0 || xa >= (STAGE_W << FP) || xb <= xa) return;
  if (xa < 0) xa = 0;
  if (xb > (STAGE_W << FP)) xb = STAGE_W << FP;
  int ia = xa >> FP, ib = xb >> FP;
  if (ia == ib) {
    edge[layer][ia] += (int16_t)(((xb - xa) * w) >> FP);
    mark(ia);
    return;
  }
  edge[layer][ia] += (int16_t)(((ONE - (xa & (ONE - 1))) * w) >> FP);
  mark(ia);
  if (ia + 1 < ib) {
    diff[layer][ia + 1] += w;
    diff[layer][ib] -= w;
    mark(ia + 1);
  }
  if (ib < STAGE_W) {
    edge[layer][ib] += (int16_t)(((xb & (ONE - 1)) * w) >> FP);
    mark(ib);
  } else {
    mark(STAGE_W);  // runs to the stage's edge: the walkers flush the run up to this mark (nothing is drawn at it)
  }
}

// Half-width of an ellipse at y (Q4), or -1 outside it.
static inline int32_t halfWidth(const QEll &o, int32_t y) {
  int32_t d = y - o.cy;
  if (d <= -o.ry || d >= o.ry) return -1;
  uint32_t s = o.ry2 - (uint32_t)(d * d);
  return (int32_t)(((int64_t)isqrt(s) * o.ratio) >> 16);
}

// What the lids leave of `shape` at y: between the cuts, minus the happy lid.
static void visible(const QEye &e, const QEll &shape, int32_t y, int cov) {
  int32_t h = halfWidth(shape, y);
  if (h < 0) return;
  int32_t l = shape.cx - h, r = shape.cx + h;
  // Upper cut: keep what lies below the line.
  const QCut &u = e.upper;
  if (u.flat) {
    if (y < u.y0) return;
  } else if (u.rising) {
    int32_t b = cutX(u, y);
    if (b < r) r = b;
  } else {
    int32_t b = cutX(u, y);
    if (b > l) l = b;
  }
  // Lower cut: keep what lies above it.
  const QCut &w = e.lower;
  if (w.flat) {
    if (y > w.y0) return;
  } else if (w.rising) {
    int32_t b = cutX(w, y);
    if (b > l) l = b;
  } else {
    int32_t b = cutX(w, y);
    if (b < r) r = b;
  }
  if (l >= r) return;
  int32_t hh = e.hasHappy ? halfWidth(e.happy, y) : -1;
  if (hh < 0) {
    addSpan(cov, l, r, W_SUB);
    return;
  }
  int32_t hl = e.happy.cx - hh, hr = e.happy.cx + hh;
  addSpan(cov, l, hl < r ? hl : r, W_SUB);
  addSpan(cov, hr > l ? hr : l, r, W_SUB);
}

// Closed eye ‿ at y: the band between the parabola ± half stroke, and round caps.
static void closedBand(const QEye &e, int32_t y) {
  int32_t lo2 = y - e.ly - e.lhw, hi2 = y - e.ly + e.lhw;  // band in curve height q(u) = b/2 − b u² / 2w²
  int64_t w2b = e.lb > 0 ? ((int64_t)e.lw * e.lw) / e.lb : 0;
  if (w2b > 0 && hi2 >= 0) {
    // |u| <= A where q(u) >= lo2, |u| >= B where q(u) <= hi2.
    int32_t A = lo2 <= 0 ? e.lw : (2 * lo2 < e.lb ? (int32_t)isqrt((uint32_t)(w2b * (e.lb - 2 * lo2))) : -1);
    int32_t B = 2 * hi2 < e.lb ? (int32_t)isqrt((uint32_t)(w2b * (e.lb - 2 * hi2))) : 0;
    if (A > e.lw) A = e.lw;
    if (A > B) {
      addSpan(LINE, e.lcx - A, e.lcx - B, e.lineW);
      if (B > 0) addSpan(LINE, e.lcx + B, e.lcx + A, e.lineW);
      else addSpan(LINE, e.lcx, e.lcx + A, e.lineW);
    }
  }
  int32_t d = y - e.ly;
  if (d > -e.lhw && d < e.lhw) {
    int32_t h = (int32_t)isqrt((uint32_t)(e.lhw * e.lhw - d * d));
    addSpan(LINE, e.lcx - e.lw - h, e.lcx - e.lw + (h < e.lw ? h : 0), e.lineW);
    addSpan(LINE, e.lcx + e.lw - (h < e.lw ? h : 0), e.lcx + e.lw + h, e.lineW);
  }
}

// Colour of a pixel from its coverage per layer (0..256, may overflow).
static inline uint16_t compose(int a, int e, int p, int l) {
  STAT(composed, 1);
  if (!(a | l)) {
    // Fast paths: most pixels are plain eye, plain pupil or empty.
    if (p >= FULL) return pupil565;
    if (p <= 0) {
      if (e >= FULL) return eye565;
      if (e <= 0) return 0;
      return be565(er * e >> 8, eg * e >> 8, eb * e >> 8);
    }
  }
  a = a < 0 ? 0 : a > FULL ? FULL : a;
  e = e < 0 ? 0 : e > FULL ? FULL : e;
  p = p < 0 ? 0 : p > FULL ? FULL : p;
  l = l < 0 ? 0 : l > FULL ? FULL : l;
  int r = er * a >> 8, g = eg * a >> 8, b = eb * a >> 8;
  r += (er - r) * e >> 8;
  g += (eg - g) * e >> 8;
  b += (eb - b) * e >> 8;
  r += (pr - r) * p >> 8;
  g += (pg - g) * p >> 8;
  b += (pb - b) * p >> 8;
  r += (er - r) * l >> 8;
  g += (eg - g) * l >> 8;
  b += (eb - b) * l >> 8;
  return be565(r, g, b);
}

// Madre's row: particles and eyes in one pass, as they have always been drawn.
static void renderRowEyes(int py, uint16_t *out) {
  lo = STAGE_W;
  hi = -1;

  for (int k = 0; k < SUB; k++) {
    int32_t y = (py << FP) + (2 * k + 1) * ONE / (2 * SUB);
    for (int i = 0; i < nDots; i++) {
      const QDot &d = dots[i];
      int32_t dy = y - d.cy;
      if (dy <= -d.r || dy >= d.r) continue;
      int32_t h = (int32_t)isqrt(d.r2 - (uint32_t)(dy * dy));
      addSpan(PART, d.cx - h, d.cx + h, d.w);
    }
    for (int i = 0; i < 2; i++) {
      const QEye &e = eyes[i];
      if (py + 1 > e.top && py < e.bottom) {
        visible(e, e.eye, y, EYE);
        visible(e, e.pupil, y, PUPIL);
      }
      if (e.line) closedBand(e, y);
    }
  }

  // Sprites (hearts, Zzz), sampled from their masks.
  for (int i = 0; i < nSprites; i++) {
    const QSprite &s = sprites[i];
    int32_t cy = (py << FP) + ONE / 2;
    if (cy < s.y0 || cy >= s.y0 + s.h) continue;
    const uint8_t *maskRow = s.mask + ((cy - s.y0) * MASK / s.h) * MASK;
    int x0 = s.x0 >> FP, x1 = (s.x0 + s.w) >> FP;
    if (x0 < 0) x0 = 0;
    if (x1 >= STAGE_W) x1 = STAGE_W - 1;
    for (int x = x0; x <= x1; x++) {
      int32_t cx = (x << FP) + ONE / 2 - s.x0;
      if (cx < 0 || cx >= s.w) continue;
      int m = maskRow[cx * MASK / s.w];
      if (!m) continue;
      edge[PART][x] += (int16_t)((m * s.a) >> 8);
      mark(x);
    }
  }

  // Composite: walk the row, recomputing only at marked pixels.
  if (hi < lo) {
    memset(out, 0, STAGE_W * sizeof(uint16_t));
    return;
  }
  if (lo > 0) memset(out, 0, lo * sizeof(uint16_t));
  if (hi < STAGE_W - 1) memset(out + hi + 1, 0, (STAGE_W - 1 - hi) * sizeof(uint16_t));
  int sa = 0, se = 0, sp = 0, sl = 0;  // running full coverage per layer
  uint16_t run = 0;
  for (int x = lo; x <= hi; x++) {
    if (!marked[x]) {
      out[x] = run;
      STAT(pixelsFilled, 1);
      continue;
    }
    marked[x] = 0;
    sa += diff[PART][x];
    se += diff[EYE][x];
    sp += diff[PUPIL][x];
    sl += diff[LINE][x];
    out[x] = compose(sa + edge[PART][x], se + edge[EYE][x], sp + edge[PUPIL][x], sl + edge[LINE][x]);
    run = compose(sa, se, sp, sl);
    for (int k = 0; k < EYE_LAYERS; k++) diff[k][x] = edge[k][x] = 0;
  }
  if (hi + 1 <= STAGE_W) {
    marked[hi + 1] = 0;
    for (int k = 0; k < EYE_LAYERS; k++) diff[k][hi + 1] = edge[k][hi + 1] = 0;
  }
}

// ─── A companion's parts ────────────────────────────────────────────────────
//
// Each part (companion.h) is its own coverage layer, drawn on top of what is
// already in the row, in order: particles, back parts, the eyes, front parts.
// Once per frame the part's points are worked out in Q4 stage pixels: the
// pack's rest points (morphed between two keys), plus each input times its
// delta set. A fill is an even-odd polygon: per sample line the edges that
// span it give crossings, which are sorted and paired into spans. A stroke is
// a union of capsules (round caps and joins): per sample line each segment
// gives the interval of its rectangle and each vertex the interval of its disc.
// All per-line work is integer; edges that cannot reach a strip of rows are
// left out of its list beforehand.

struct Pt {
  int16_t x, y;  // Q4 stage px
};
struct Aux {  // a stroke segment: the normal of half the line's width, and the slope of its end caps
  int16_t nx, ny;
  int32_t cap;
};
struct QSub {
  uint16_t first, n;  // n includes the closing copy of a closed sub-path; 0 = not drawn
  bool closed;
  uint8_t eye;  // lid-clipped parts: 1 left, 2 right
};
// Flags of a strip list entry: a stroke's point has no segment / no disc; a clipped fill's edge has its eye in the top bits.
enum : uint16_t { NO_SEG = 0x8000, NO_DISC = 0x4000, ITEM_MASK = 0x3FFF, EYE_SHIFT = 14 };

struct QPart {
  bool stroke, clip;
  uint8_t layer;
  int r, g, b, op;  // paint, opacity 0..256
  int32_t rad, rad2;  // half the line's width (Q4)
  int32_t top, bottom;  // stage rows covered
  uint16_t sub0, nSub, pt0, aux0;
  uint16_t item0, nItems;  // this strip's list
  const uint16_t *disc;    // half-width of a vertex's disc at |dy| = 0 … rad − 1 (Q4), or null
};

// 1/16 design unit to Q4 stage px, Q16.
static constexpr int32_t KPX = (int32_t)(STAGE_SCALE * 65536 + 0.5f);

static Pt pts[COMP_MAX_POINTS];
static int32_t slope[COMP_MAX_POINTS];  // dx / dy of the edge from point i to i + 1, Q16
static Aux aux[COMP_MAX_STROKE_POINTS];
static uint16_t items[COMP_MAX_POINTS];
static QSub qsubs[COMP_MAX_SUBS];
static QPart qparts[COMP_MAX_PARTS];
static int nQParts = 0, nPts = 0, nQSubs = 0, nAux = 0;
static uint16_t discPool[1024];  // the discs' half-widths, one table per stroked part
static int nDisc = 0;
static uint16_t rowItems[COMP_MAX_POINTS];  // the strip list cut down to one row

static void prepareParts(const StageFrame &f) {
  nQParts = nPts = nQSubs = nAux = nDisc = 0;
  if (!f.comp.c) return;
  const CompDraw &cd = f.comp;
  const Companion &c = *cd.c;
  int mixQ = (int)lroundf(clampf(cd.mix, 0, 1) * 256);
  const int32_t ox = q(STAGE_OX), oy = q(STAGE_OY + cd.dy * STAGE_SCALE);
  const uint32_t rgbOf[5] = {f.eye, f.pupil, cd.accent, cd.accentSoft, 0};
  const float perUnit = STAGE_SCALE * 16 * 256 / 127.0f;  // i8 × scale → Q4 px, in Q8

  for (int pi = 0; pi < c.nParts; pi++) {
    const CompPart &cp = c.parts[pi];
    if (!cp.nPoints || !cp.nSub) continue;
    // The inputs. A part that limits the gaze to the unit disc (a pupil) keeps it there.
    float gx = cd.gazeX, gy = cd.gazeY;
    if (cp.gazeDisc) {
      float m = sqrtf(gx * gx + gy * gy);
      if (m > 1) gx /= m, gy /= m;
    }
    const float amount[SETS] = {gx, gy, cd.breath - 0.5f, cd.bounce};
    PartKey ka, kb;
    companionPartKey(cp, cd.stateA, ka);
    companionPartKey(cp, cd.stateB, kb);
    // The key to draw, the one it is morphing to (if any) and its weight (Q8).
    const PartKey *k0 = &ka, *k1 = nullptr;
    int w1 = 0;
    if (mixQ >= 256) k0 = &kb;
    else if (mixQ > 0 && ka.pts != kb.pts) k1 = &kb, w1 = mixQ;
    int opacity = k1 ? k0->opacity + (((int)k1->opacity - (int)k0->opacity) * w1 >> 8) : k0->opacity;
    if (opacity <= 0) continue;

    const CompSub *cs = c.subs + cp.firstSub;
    int closed = 0;
    for (int j = 0; j < cp.nSub; j++) closed += cs[j].closed && cs[j].n;
    int total = cp.nPoints + closed;
    bool stroke = cp.stroke > 0;
    if (nQParts >= COMP_MAX_PARTS || nPts + total > COMP_MAX_POINTS || nQSubs + cp.nSub > COMP_MAX_SUBS) continue;
    if (stroke && nAux + total > COMP_MAX_STROKE_POINTS) continue;

    // Rest points, blended between the two keys.
    Pt *dst = pts + nPts;
    for (int j = 0; j < cp.nPoints; j++) {
      int x = rdI16(k0->pts + 4 * j), y = rdI16(k0->pts + 4 * j + 2);
      if (k1) {
        x += ((rdI16(k1->pts + 4 * j) - x) * w1) >> 8;
        y += ((rdI16(k1->pts + 4 * j + 2) - y) * w1) >> 8;
      }
      int32_t px = ox + (int32_t)(((int64_t)x * KPX + 32768) >> 16), py = oy + (int32_t)(((int64_t)y * KPX + 32768) >> 16);
      dst[j].x = (int16_t)(px < -12000 ? -12000 : px > 12000 ? 12000 : px);
      dst[j].y = (int16_t)(py < -12000 ? -12000 : py > 12000 ? 12000 : py);
    }
    STAT(pointOps, cp.nPoints * (k1 ? 3 : 2));
    // Inputs times deltas, each key weighted by its share of the morph.
    int16_t *flat = &dst[0].x;
    for (int pass = 0; pass < 2; pass++) {
      const PartKey *k = pass ? k1 : k0;
      if (!k) continue;
      int w = pass ? w1 : (k1 ? 256 - w1 : 256);
      for (int s = 0; s < SETS; s++) {
        if (!k->delta[s]) continue;
        float cf = amount[s] * k->scale[s] * perUnit * w / 256.0f;
        if (!(cf > -200000.0f && cf < 200000.0f)) cf = 0;  // NaN and absurd scales (a damaged pack) move nothing
        int32_t coef = (int32_t)lroundf(cf);
        if (!coef) continue;
        const uint8_t *d = k->delta[s];
        for (int i = 0; i < 2 * cp.nPoints; i++) {
          int32_t v = flat[i] + (((int8_t)d[i] * coef + 128) >> 8);
          flat[i] = (int16_t)(v < -12000 ? -12000 : v > 12000 ? 12000 : v);
        }
        STAT(pointOps, 2 * cp.nPoints);
      }
    }

    // Keep every point within ±750 px: far more than the stage, and small enough that no edge's
    // slope times its height can overflow (a damaged pack must not do undefined things).
    for (int i = 0; i < 2 * cp.nPoints; i++) flat[i] = flat[i] < -12000 ? -12000 : flat[i] > 12000 ? 12000 : flat[i];

    // Sub-paths: a closed one gets a copy of its first point at the end, so
    // every edge is point i to i + 1. Moved from the last one backwards, in place.
    QPart &qp = qparts[nQParts];
    qp.stroke = stroke;
    qp.clip = cp.clip;
    qp.layer = cp.layer;
    qp.sub0 = nQSubs;
    qp.nSub = cp.nSub;
    qp.pt0 = nPts;
    qp.aux0 = nAux;
    qp.rad = stroke ? (int32_t)lroundf(cp.stroke * 0.5f * STAGE_SCALE * ONE) : 0;
    if (stroke && qp.rad < 1) qp.rad = 1;
    qp.rad2 = qp.rad * qp.rad;
    // A stroke's discs are looked up, not worked out: sqrt(rad² − dy²) for every whole dy.
    qp.disc = nullptr;
    if (stroke && nDisc + qp.rad <= (int)(sizeof discPool / sizeof discPool[0])) {
      uint16_t *t = discPool + nDisc;
      for (int d = 0; d < qp.rad; d++) t[d] = (uint16_t)isqrt((uint32_t)(qp.rad2 - d * d));
      qp.disc = t;
      nDisc += qp.rad;
    }
    int oEnd = cp.nPoints, tEnd = total;
    for (int j = cp.nSub - 1; j >= 0; j--) {
      int n = cs[j].n, cl = cs[j].closed && n ? 1 : 0;
      int o = oEnd - n, t = tEnd - n - cl;
      if (t != o) memmove(dst + t, dst + o, n * sizeof(Pt));
      if (cl) dst[t + n] = dst[t];
      qsubs[nQSubs + j] = {(uint16_t)(nPts + t), (uint16_t)(n + cl), cs[j].closed, cs[j].eye};
      oEnd = o;
      tEnd = t;
    }
    // Bounds, and which sub-paths are worth drawing (a sub-path that has collapsed onto one point is not).
    int32_t ymin = INT32_MAX, ymax = INT32_MIN;
    for (int j = 0; j < cp.nSub; j++) {
      QSub &qs = qsubs[nQSubs + j];
      int need = stroke ? 2 : 3;
      if (qs.n < need + (qs.closed ? 1 : 0)) {
        qs.n = 0;
        continue;
      }
      const Pt *p0 = pts + qs.first;
      bool same = true;
      for (int i = 0; i < qs.n; i++) {
        if (p0[i].x != p0->x || p0[i].y != p0->y) same = false;
        if (p0[i].y < ymin) ymin = p0[i].y;
        if (p0[i].y > ymax) ymax = p0[i].y;
      }
      if (same) qs.n = 0;
    }
    if (ymin > ymax) continue;
    qp.top = (ymin - qp.rad) >> FP;
    qp.bottom = ((ymax + qp.rad) >> FP) + 1;
    if (qp.bottom <= 0 || qp.top >= STAGE_H) continue;

    // Edge slopes, and for a stroke the normals and cap slopes of its segments.
    for (int j = 0; j < cp.nSub; j++) {
      const QSub &qs = qsubs[nQSubs + j];
      for (int i = qs.first; i + 1 < qs.first + qs.n; i++) {
        int32_t dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
        slope[i] = dy ? (int32_t)(((int64_t)dx * 65536) / dy) : 0;
        STAT(divs, 1);
        if (!stroke) continue;
        Aux &u = aux[qp.aux0 + (i - qp.pt0)];
        uint32_t len = isqrt((uint32_t)(dx * dx + dy * dy));
        if (!len) {
          u = {0, 0, 0};
          continue;
        }
        u.nx = (int16_t)(-dy * qp.rad / (int32_t)len);
        u.ny = (int16_t)(dx * qp.rad / (int32_t)len);
        u.cap = u.ny ? (int32_t)(((int64_t)u.nx * 65536) / u.ny) : 0;
        STAT(divs, 2);
      }
    }

    int p = cp.paint;
    qp.r = (rgbOf[p] >> 16) & 0xFF;
    qp.g = (rgbOf[p] >> 8) & 0xFF;
    qp.b = rgbOf[p] & 0xFF;
    qp.op = opacity + (opacity >> 7);
    nPts += total;
    nQSubs += cp.nSub;
    if (stroke) nAux += total;
    nQParts++;
  }
}

// The entries of each part's list for the rows [y0, y0 + rows): only the edges (or capsules) that can touch them.
static void buildStrip(int y0, int rows) {
  const int32_t ys = y0 << FP, ye = (y0 + rows) << FP;
  int n = 0;
  for (int pi = 0; pi < nQParts; pi++) {
    QPart &qp = qparts[pi];
    qp.item0 = n;
    if (qp.top >= y0 + rows || qp.bottom <= y0) {
      qp.nItems = 0;
      continue;
    }
    for (int j = 0; j < qp.nSub; j++) {
      const QSub &qs = qsubs[qp.sub0 + j];
      for (int i = qs.first, end = qs.first + qs.n; i < end; i++) {
        bool last = i == end - 1;
        int32_t a = pts[i].y, b = last ? a : pts[i + 1].y;
        int32_t lo2 = a < b ? a : b, hi2 = a < b ? b : a;
        uint16_t flags = 0;
        if (qp.stroke) {
          if (last) flags = qs.closed ? NO_SEG | NO_DISC : NO_SEG;
          if (hi2 + qp.rad <= ys || lo2 - qp.rad >= ye) continue;
        } else {
          // A fill needs only edges that cross: they span [lo, hi) of sample lines.
          if (last || a == b || hi2 <= ys || lo2 >= ye) continue;
          if (qp.clip) flags = (uint16_t)(qs.eye << EYE_SHIFT);
        }
        items[n++] = (uint16_t)(i | flags);
      }
    }
    qp.nItems = n - qp.item0;
  }
}

// The lids of an eye cut a span at sample line clipY (the upper and lower cuts keep what lies between
// them, as visible() does for the eye's own ellipse), and the happy lid bites a piece out of it.
static int32_t clipY = 0;
static void clipSpan(const QEye &e, int32_t l, int32_t r) {
  int32_t y = clipY;
  const QCut &u = e.upper;
  if (u.flat) {
    if (y < u.y0) return;
  } else if (u.rising) {
    int32_t b = cutX(u, y);
    if (b < r) r = b;
  } else {
    int32_t b = cutX(u, y);
    if (b > l) l = b;
  }
  const QCut &w = e.lower;
  if (w.flat) {
    if (y > w.y0) return;
  } else if (w.rising) {
    int32_t b = cutX(w, y);
    if (b > l) l = b;
  } else {
    int32_t b = cutX(w, y);
    if (b < r) r = b;
  }
  if (l >= r) return;
  int32_t hh = e.hasHappy ? halfWidth(e.happy, y) : -1;
  if (hh < 0) {
    addSpan(GEN, l, r, W_SUB);
    return;
  }
  int32_t hl = e.happy.cx - hh, hr = e.happy.cx + hh;
  addSpan(GEN, l, hl < r ? hl : r, W_SUB);
  addSpan(GEN, hr > l ? hr : l, r, W_SUB);
}

// Even-odd spans of the crossings in xs[0, n): sorted and paired. A clipped part's spans are
// then cut by the lids of `eye` (1 left, 2 right) and the happy lid's bite, as an eye's own are.
static void spansOf(int32_t *xs, int n, int eye) {
  for (int i = 1; i < n; i++) {  // insertion sort: a handful of crossings
    int32_t v = xs[i];
    int j = i - 1;
    for (; j >= 0 && xs[j] > v; j--) xs[j + 1] = xs[j];
    xs[j + 1] = v;
  }
  for (int i = 0; i + 1 < n; i += 2) {
    int32_t a = xs[i], b = xs[i + 1];
    if (a < b) {
      if (eye) clipSpan(eyes[eye - 1], a, b);
      else addSpan(GEN, a, b, W_SUB);
    }
  }
}

static void gatherFill(int nRow, int32_t y) {
  int32_t xs[3][48];
  int n[3] = {0, 0, 0};
  for (int k = 0; k < nRow; k++) {
    int i = rowItems[k] & ITEM_MASK, g = rowItems[k] >> EYE_SHIFT;
    STAT(edgeTests, 1);
    if ((pts[i].y <= y) != (pts[i + 1].y <= y)) {
      STAT(crossings, 1);
      if (n[g] < 48) xs[g][n[g]++] = pts[i].x + (((y - pts[i].y) * slope[i]) >> 16);
    }
  }
  clipY = y;
  for (int g = 0; g < 3; g++) spansOf(xs[g], n[g], g);
}

// Edge from (px, py) to (·, qy) with slope s (dx / dy, Q16): its x at y if it crosses, folded into [lo, hi].
static inline void cross(int32_t px, int32_t py, int32_t qy, int32_t s, int32_t y, int32_t &lo, int32_t &hi) {
  STAT(edgeTests, 1);
  if ((py <= y) == (qy <= y)) return;
  STAT(crossings, 1);
  int32_t x = px + (((y - py) * s) >> 16);
  if (x < lo) lo = x;
  if (x > hi) hi = x;
}

static void gatherStroke(const QPart &qp, int nRow, int32_t y) {
  for (int k = 0; k < nRow; k++) {
    uint16_t it = rowItems[k];
    int i = it & ITEM_MASK;
    const Pt &a = pts[i];
    if (!(it & NO_DISC)) {
      int32_t dy = y - a.y;
      if (dy > -qp.rad && dy < qp.rad) {
        int32_t h = qp.disc ? qp.disc[dy < 0 ? -dy : dy] : (int32_t)isqrt(qp.rad2 - (uint32_t)(dy * dy));
        addSpan(GEN, a.x - h, a.x + h, W_SUB);
      }
    }
    if (it & NO_SEG) continue;
    const Pt &b = pts[i + 1];
    const Aux &u = aux[qp.aux0 + (i - qp.pt0)];
    // The segment's rectangle: corners A ± n and B ± n.
    int32_t lo = INT32_MAX, hi = INT32_MIN, s = slope[i];
    int32_t p1x = a.x + u.nx, p1y = a.y + u.ny, p2x = b.x + u.nx, p2y = b.y + u.ny;
    int32_t p3x = b.x - u.nx, p3y = b.y - u.ny, p4x = a.x - u.nx, p4y = a.y - u.ny;
    cross(p1x, p1y, p2y, s, y, lo, hi);
    cross(p2x, p2y, p3y, u.cap, y, lo, hi);
    cross(p3x, p3y, p4y, s, y, lo, hi);
    cross(p4x, p4y, p1y, u.cap, y, lo, hi);
    if (hi > lo) addSpan(GEN, lo, hi, W_SUB);
  }
}

// ─── Blending a layer onto the row ──────────────────────────────────────────

static inline void unpack565(uint16_t be, int &r, int &g, int &b) {
  uint16_t c = (uint16_t)((be >> 8) | (be << 8));
  r = (c >> 8) & 0xF8;
  g = (c >> 3) & 0xFC;
  b = (c << 3) & 0xF8;
  r |= r >> 5;
  g |= g >> 6;
  b |= b >> 5;
}

static inline int clampCov(int v) {
  return v < 0 ? 0 : v > FULL ? FULL : v;
}

// Next marked pixel in [x, hi], or hi + 1: unmarked stretches are skipped a word at a time.
static inline int nextMarked(int x) {
  while (x <= hi) {
    if (marked[x]) return x;
    if (!(x & 3)) {
      uint32_t w;
      memcpy(&w, marked + x, 4);
      if (!w) {
        x += 4;
        continue;
      }
    }
    x++;
  }
  return hi + 1;
}

static inline void fill16(uint16_t *p, int n, uint16_t v) {
  STAT(pixelsFilled32, n);
  if (n > 3 && ((uintptr_t)p & 2)) {
    *p++ = v;
    n--;
  }
  uint32_t w = (uint32_t)v | ((uint32_t)v << 16);
  for (; n >= 2; n -= 2, p += 2) memcpy(p, &w, 4);
  if (n) *p = v;
}

// `col` over the pixel at coverage `a` (0..256).
static inline void blendPixel(uint16_t &px, int a, int cr, int cg, int cb, uint16_t col) {
  STAT(pixelsBlended, 1);
  if (a >= FULL) {
    px = col;
    return;
  }
  int r, g, b;
  unpack565(px, r, g, b);
  r += (cr - r) * a >> 8;
  g += (cg - g) * a >> 8;
  b += (cb - b) * a >> 8;
  px = be565r(r, g, b);
}

static void blendRun(uint16_t *p, int n, int cov, int cr, int cg, int cb, uint16_t col) {
  if (cov >= FULL) {
    fill16(p, n, col);
    return;
  }
  // Most of a run lies over one colour (black, or an earlier part's): work it out once for each.
  uint16_t seen = p[0], made = 0;
  bool have = false;
  for (int i = 0; i < n; i++) {
    if (have && p[i] == seen) {
      p[i] = made;
      continue;
    }
    seen = p[i];
    blendPixel(p[i], cov, cr, cg, cb, col);
    made = p[i];
    have = true;
  }
}

// Walks the marked range of `layer` and draws the colour over the row at its coverage times `op`; clears the layer.
static void blendLayer(int layer, int cr, int cg, int cb, int op, uint16_t *out) {
  if (hi < lo) return;
  const uint16_t col = be565r(cr, cg, cb);
  int s = 0, runStart = lo;
  for (int x = nextMarked(lo); x <= hi; x = nextMarked(x + 1)) {
    STAT(pixelsWalked, 1);
    marked[x] = 0;
    int run = clampCov(s) * op >> 8;
    if (run > 0 && x > runStart) blendRun(out + runStart, x - runStart, run, cr, cg, cb, col);
    if (x >= STAGE_W) {
      diff[layer][x] = 0;
      break;
    }
    s += diff[layer][x];
    int a = clampCov(s + edge[layer][x]) * op >> 8;
    if (a > 0) blendPixel(out[x], a, cr, cg, cb, col);
    diff[layer][x] = edge[layer][x] = 0;
    runStart = x + 1;
  }
  if (hi < STAGE_W) {
    marked[hi + 1] = 0;
    diff[layer][hi + 1] = edge[layer][hi + 1] = 0;
  }
  lo = STAGE_W;
  hi = -1;
}

// The eyes' layers (eye, pupil, closed line) over what is in the row.
static inline void eyePixel(uint16_t &px, int e, int p, int l) {
  e = clampCov(e);
  p = clampCov(p);
  l = clampCov(l);
  STAT(pixelsBlended, 1);
  if (!l) {
    if (p >= FULL) {
      px = pupil565r;
      return;
    }
    if (!p) {
      if (e >= FULL) px = eye565r;
      else if (e) {
        int r, g, b;
        unpack565(px, r, g, b);
        px = be565r(r + ((er - r) * e >> 8), g + ((eg - g) * e >> 8), b + ((eb - b) * e >> 8));
      }
      return;
    }
  }
  int r, g, b;
  unpack565(px, r, g, b);
  r += (er - r) * e >> 8;
  g += (eg - g) * e >> 8;
  b += (eb - b) * e >> 8;
  r += (pr - r) * p >> 8;
  g += (pg - g) * p >> 8;
  b += (pb - b) * p >> 8;
  r += (er - r) * l >> 8;
  g += (eg - g) * l >> 8;
  b += (eb - b) * l >> 8;
  px = be565r(r, g, b);
}

static void eyesOver(uint16_t *out) {
  if (hi < lo) return;
  int se = 0, sp = 0, sl = 0, runStart = lo;
  for (int x = nextMarked(lo); x <= hi; x = nextMarked(x + 1)) {
    STAT(pixelsWalked, 1);
    marked[x] = 0;
    int n = x - runStart;
    if (n > 0 && (se > 0 || sp > 0 || sl > 0)) {
      int e = clampCov(se), p = clampCov(sp), l = clampCov(sl);
      if (!l && p >= FULL) fill16(out + runStart, n, pupil565r);
      else if (!l && !p && e >= FULL) fill16(out + runStart, n, eye565r);
      else
        for (int i = runStart; i < x; i++) eyePixel(out[i], e, p, l);
    }
    if (x >= STAGE_W) {
      for (int k = EYE; k <= LINE; k++) diff[k][x] = 0;
      break;
    }
    se += diff[EYE][x];
    sp += diff[PUPIL][x];
    sl += diff[LINE][x];
    eyePixel(out[x], se + edge[EYE][x], sp + edge[PUPIL][x], sl + edge[LINE][x]);
    for (int k = EYE; k <= LINE; k++) diff[k][x] = edge[k][x] = 0;
    runStart = x + 1;
  }
  if (hi < STAGE_W) {
    marked[hi + 1] = 0;
    for (int k = EYE; k <= LINE; k++) diff[k][hi + 1] = edge[k][hi + 1] = 0;
  }
  lo = STAGE_W;
  hi = -1;
}

// ─── Rows ───────────────────────────────────────────────────────────────────

static void partRow(const QPart &qp, int py, uint16_t *out) {
  if (!qp.nItems || py + 1 <= qp.top || py >= qp.bottom) return;
  // Of the strip's list, what can touch this row.
  const int32_t ys = py << FP, ye = ys + ONE, pad = qp.rad;
  int nRow = 0;
  for (int k = qp.item0, end = k + qp.nItems; k < end; k++) {
    int i = items[k] & ITEM_MASK;
    int32_t a = pts[i].y, b = (qp.stroke && (items[k] & NO_SEG)) ? a : pts[i + 1].y;
    if (a > b) std::swap(a, b);
    if (b + pad > ys && a - pad < ye) rowItems[nRow++] = items[k];
  }
  if (!nRow) return;
  lo = STAGE_W;
  hi = -1;
  for (int k = 0; k < SUB; k++) {
    int32_t y = (py << FP) + (2 * k + 1) * ONE / (2 * SUB);
    if (qp.stroke) gatherStroke(qp, nRow, y);
    else gatherFill(nRow, y);
  }
  blendLayer(GEN, qp.r, qp.g, qp.b, qp.op, out);
}

// A row with a companion: black, particles, back parts, eyes, front parts.
static void renderRowParts(int py, uint16_t *out) {
  memset(out, 0, STAGE_W * sizeof(uint16_t));
  lo = STAGE_W;
  hi = -1;
  for (int k = 0; k < SUB; k++) {
    int32_t y = (py << FP) + (2 * k + 1) * ONE / (2 * SUB);
    for (int i = 0; i < nDots; i++) {
      const QDot &d = dots[i];
      int32_t dy = y - d.cy;
      if (dy <= -d.r || dy >= d.r) continue;
      int32_t h = (int32_t)isqrt(d.r2 - (uint32_t)(dy * dy));
      addSpan(PART, d.cx - h, d.cx + h, d.w);
    }
  }
  for (int i = 0; i < nSprites; i++) {
    const QSprite &s = sprites[i];
    int32_t cy = (py << FP) + ONE / 2;
    if (cy < s.y0 || cy >= s.y0 + s.h) continue;
    const uint8_t *maskRow = s.mask + ((cy - s.y0) * MASK / s.h) * MASK;
    int x0 = s.x0 >> FP, x1 = (s.x0 + s.w) >> FP;
    if (x0 < 0) x0 = 0;
    if (x1 >= STAGE_W) x1 = STAGE_W - 1;
    for (int x = x0; x <= x1; x++) {
      int32_t cx = (x << FP) + ONE / 2 - s.x0;
      if (cx < 0 || cx >= s.w) continue;
      int m = maskRow[cx * MASK / s.w];
      if (!m) continue;
      edge[PART][x] += (int16_t)((m * s.a) >> 8);
      mark(x);
    }
  }
  blendLayer(PART, er, eg, eb, FULL, out);

  for (int i = 0; i < nQParts; i++)
    if (qparts[i].layer == 0) partRow(qparts[i], py, out);
  if (eyesOn) {
    for (int k = 0; k < SUB; k++) {
      int32_t y = (py << FP) + (2 * k + 1) * ONE / (2 * SUB);
      for (int i = 0; i < 2; i++) {
        const QEye &e = eyes[i];
        if (py + 1 > e.top && py < e.bottom) {
          visible(e, e.eye, y, EYE);
          visible(e, e.pupil, y, PUPIL);
        }
        if (e.line) closedBand(e, y);
      }
    }
    eyesOver(out);
  }
  for (int i = 0; i < nQParts; i++)
    if (qparts[i].layer == 1) partRow(qparts[i], py, out);
}

void renderStage(int y0, int rows, uint16_t *buf) {
  bool parts = nQParts > 0 || !eyesOn;
  if (parts) buildStrip(y0, rows);
  for (int row = 0; row < rows; row++) {
    uint16_t *out = buf + row * STAGE_W;
    if (parts) renderRowParts(y0 + row, out);
    else renderRowEyes(y0 + row, out);
  }
}
