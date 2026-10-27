#include "stage.h"
#include <math.h>
#include <string.h>

// Fixed point: coordinates in 1/16 px (Q4).
static constexpr int FP = 4;
static constexpr int32_t ONE = 1 << FP;
static constexpr int SUB = 4;  // vertical samples per row
static constexpr int FULL = 256;
static constexpr int W_SUB = FULL / SUB;
static constexpr float LINE_HALF = 3;  // closed line: half stroke, design units
static constexpr int MASK = 32;        // sprite masks are MASK × MASK

static inline int32_t q(float v) {
  return (int32_t)lroundf(v * ONE);
}

static uint32_t isqrt(uint32_t v) {
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

static QEye eyes[2];
static QDot dots[MAX_DOTS];
static int nDots = 0;
static QSprite sprites[6];
static int nSprites = 0;
static uint16_t eye565, pupil565;
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

enum Layer { PART, EYE, PUPIL, LINE, LAYERS };
static int16_t diff[LAYERS][STAGE_W + 1], edge[LAYERS][STAGE_W + 1];
static uint8_t marked[STAGE_W + 1];
static int lo, hi;

static inline void mark(int x) {
  marked[x] = 1;
  if (x < lo) lo = x;
  if (x > hi) hi = x;
}

// Adds weight `w` over [xa, xb) (Q4) to a layer, with fractional end pixels.
static void addSpan(int layer, int32_t xa, int32_t xb, int w) {
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

void renderStage(int y0, int rows, uint16_t *buf) {
  for (int row = 0; row < rows; row++) {
    int py = y0 + row;
    uint16_t *out = buf + row * STAGE_W;
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
      continue;
    }
    if (lo > 0) memset(out, 0, lo * sizeof(uint16_t));
    if (hi < STAGE_W - 1) memset(out + hi + 1, 0, (STAGE_W - 1 - hi) * sizeof(uint16_t));
    int sa = 0, se = 0, sp = 0, sl = 0;  // running full coverage per layer
    uint16_t run = 0;
    for (int x = lo; x <= hi; x++) {
      if (!marked[x]) {
        out[x] = run;
        continue;
      }
      marked[x] = 0;
      sa += diff[PART][x];
      se += diff[EYE][x];
      sp += diff[PUPIL][x];
      sl += diff[LINE][x];
      out[x] = compose(sa + edge[PART][x], se + edge[EYE][x], sp + edge[PUPIL][x], sl + edge[LINE][x]);
      run = compose(sa, se, sp, sl);
      for (int k = 0; k < LAYERS; k++) diff[k][x] = edge[k][x] = 0;
    }
    if (hi + 1 <= STAGE_W) {
      marked[hi + 1] = 0;
      for (int k = 0; k < LAYERS; k++) diff[k][hi + 1] = edge[k][hi + 1] = 0;
    }
  }
}
