#include "pet.h"
#include <math.h>
#include <string.h>
#include <initializer_list>

// Timings and springs from PetEyes.tsx / PetScreen.tsx.
static constexpr uint32_t EXPR_MS = 320;
static constexpr uint32_t ALIVE_MS = 600;
static constexpr uint32_t COLOR_MS = 450;
static constexpr uint32_t TRAIL_MS = 28;
static constexpr uint32_t LINE_MS = 4200;
static constexpr uint32_t CHAT_MS = 45000;
static constexpr uint32_t FEED_MS = 2800;
static constexpr uint32_t TAP_MS = 450;
static constexpr float TAP_PX = 24;  // watch pixels are about half a phone point
static constexpr float SWIPE_PX = 70;
static constexpr uint32_t SWIPE_MS = 700;
static constexpr uint32_t DOUBLE_TAP_MS = 400;
static constexpr uint32_t LOVE_MS = 2600;
static constexpr uint32_t ANNOY_WINDOW_MS = 2500;
static constexpr uint32_t FAR = 0x40000000;
// On the watch, gestures are the main event: idle glances come less often and
// wander less than in the app, so they are not mistaken for reactions.
static constexpr float GLANCE_SLOWER = 1.8f;
static constexpr float GLANCE_NARROWER = 0.7f;
struct SpringParams {
  float damping, stiffness, mass;
};
static constexpr SpringParams SACCADE = {15, 320, 0.7f};
static constexpr SpringParams SOFT = {26, 90, 1};

// PetParticles.tsx layouts.
struct BubbleDef {
  float x, r, phase, speed;
};
static const BubbleDef BUBBLES[] = {{0.12f, 5, 0, 1}, {0.27f, 9, 0.35f, 0.8f}, {0.46f, 4, 0.7f, 1.2f}, {0.63f, 7, 0.15f, 0.9f}, {0.8f, 11, 0.55f, 0.75f}, {0.92f, 5, 0.85f, 1.1f}};
static const float FLOUR[] = {0.3f, 0.38f, 0.45f, 0.5f, 0.55f, 0.62f, 0.7f, 0.42f, 0.58f, 0.48f};
struct HeartDef {
  float x, delay, size;
};
static const HeartDef HEARTS[] = {{0.3f, 0, 14}, {0.5f, 0.12f, 20}, {0.7f, 0.24f, 13}};

static float clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}
static float easeOutQuint(float t) {
  float u = 1 - t;
  return 1 - u * u * u * u * u;
}
static float easeInOutQuad(float t) {
  return t < 0.5f ? 2 * t * t : 1 - (2 - 2 * t) * (2 - 2 * t) / 2;
}
static float easeOutQuad(float t) {
  return 1 - (1 - t) * (1 - t);
}
static float easeOutCubic(float t) {
  float u = 1 - t;
  return 1 - u * u * u;
}
static float mix(float a, float b, float k) {
  return a + (b - a) * k;
}
static uint32_t lerpColor(uint32_t a, uint32_t b, float k) {
  uint32_t out = 0;
  for (int s = 0; s <= 16; s += 8) out |= (uint32_t)lroundf(mix((a >> s) & 0xFF, (b >> s) & 0xFF, k)) << s;
  return out;
}
// Wrap-safe "time has come".
static bool due(uint32_t now, uint32_t at) {
  return (int32_t)(now - at) >= 0;
}
static bool persistentMood(Mood m) {
  return m == NEWBORN || m == HUNGRY || m == STARVING || m == DORMANT;
}

void Spring::step(float dt) {
  // Semi-implicit Euler in small steps: stable for these stiff springs.
  const float h = 0.002f;
  while (dt > 0) {
    float s = dt < h ? dt : h;
    float a = (-stiffness * (x - target) - damping * v) / mass;
    v += a * s;
    x += v * s;
    dt -= s;
  }
}

uint32_t Pet::rand32() {
  // xorshift32
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

void Pet::begin(uint32_t ms, uint32_t seed) {
  rng_ = seed | 1;
  t0_ = now_ = ms;
  cur_ = from_ = to_ = EXPRESSIONS[E_NEUTRAL];
  Palette p = paletteFor(NEWBORN, false, E_NEUTRAL);
  eye_ = eyeFrom_ = eyeTo_ = p.eye;
  pupil_ = pupilFrom_ = pupilTo_ = p.pupil;
  scheduleBlink(ms);
}

void Pet::setState(Mood mood, bool asleep, uint32_t ms) {
  if (hasState_ && mood == mood_ && asleep == asleep_) return;
  bool first = !hasState_;
  mood_ = mood;
  asleep_ = asleep;
  hasState_ = true;
  live_ = livelinessFor(mood, asleep);

  bubblesFrom_ = bubbles_;
  bubblesTo_ = live_.bubbleSeconds > 0 ? 1 : 0;
  bubblesStart_ = ms;
  if (live_.bubbleSeconds > 0) period_ = live_.bubbleSeconds;

  if (first) {
    // Start already in the right face and colours.
    expr_ = expressionFor(mood, asleep);
    cur_ = from_ = to_ = EXPRESSIONS[expr_];
    alive_ = aliveFrom_ = aliveTo_ = to_.lid >= 1 ? 0 : 1;
    Palette p = paletteFor(mood, asleep, expr_);
    eye_ = eyeFrom_ = eyeTo_ = p.eye;
    pupil_ = pupilFrom_ = pupilTo_ = p.pupil;
    bubbles_ = bubblesFrom_ = bubblesTo_;
    zzz_ = zzzFrom_ = zzzTo_ = asleep || mood == DORMANT ? 1 : 0;
  }

  say(asleep ? L_ASLEEP : (LineKey)mood, persistentMood(mood), ms);
  nextLine_ = ms + CHAT_MS;
  nextQuirk_ = ms + 9000 + (uint32_t)(rand01() * 7000);
  scheduleGlance(ms);
}

ExprName Pet::currentExpr() const {
  return steps_ > 0 ? seq_[stepIndex_].expr : expressionFor(mood_, asleep_);
}

void Pet::setExpression(ExprName e, uint32_t ms) {
  from_ = cur_;
  to_ = EXPRESSIONS[e];
  expr_ = e;
  exprStart_ = ms;
  aliveFrom_ = alive_;
  aliveTo_ = to_.lid >= 1 ? 0 : 1;
}

void Pet::look(float x, float y, float damping, float stiffness, float mass, uint32_t ms) {
  for (Spring *s : {&lookRX_, &lookRY_, &lookLX_, &lookLY_}) {
    s->damping = damping;
    s->stiffness = stiffness;
    s->mass = mass;
  }
  lookRX_.target = x;
  lookRY_.target = y;
  // The left eye follows a few milliseconds later: secondary motion.
  leftTX_ = x;
  leftTY_ = y;
  leftAt_ = ms + TRAIL_MS;
  leftPending_ = true;
}

void Pet::scheduleGlance(uint32_t ms) {
  if (live_.glanceSeconds <= 0) {
    if (!gazeHeld_ && !touching_) look(0, 0, SOFT.damping, SOFT.stiffness, SOFT.mass, ms);
    nextGlance_ = ms + FAR;
    return;
  }
  nextGlance_ = ms + (uint32_t)(live_.glanceSeconds * GLANCE_SLOWER * 1000 * (0.55f + rand01() * 0.9f));
}

void Pet::scheduleBlink(uint32_t ms) {
  nextBlink_ = ms + 2500 + (uint32_t)(rand01() * 3500);
}

void Pet::bounce(uint32_t ms, float amp) {
  // Start from wherever the last hop is, so repeated hops never jump.
  bounceFrom_ = bounceMode_ == 1 ? bounceFrom_ + (bounceAmp_ - bounceFrom_) * easeOutQuad(clamp01((ms - bounceStart_) / 110.0f)) : bounceMode_ == 2 ? bounce_.x : 0;
  bounceAmp_ = amp;
  bounceMode_ = 1;
  bounceStart_ = ms;
}

void Pet::say(LineKey key, bool persistent, uint32_t ms) {
  line_ = petLine(key, rand32());
  lineId_++;
  lineUntil_ = persistent ? 0 : ms + LINE_MS;
  if (!persistent && lineUntil_ == 0) lineUntil_ = 1;
}

void Pet::play(const Step *steps, int n, uint32_t ms) {
  memcpy(seq_, steps, sizeof(Step) * n);
  steps_ = n;
  stepIndex_ = 0;
  startStep(ms);
}

void Pet::startStep(uint32_t ms) {
  stepStart_ = ms;
  const Step &s = seq_[stepIndex_];
  if (s.hasGaze) {
    gazeHeld_ = true;
    look(s.gx, s.gy, SACCADE.damping, SACCADE.stiffness, SACCADE.mass, ms);
  } else if (gazeHeld_) {
    gazeHeld_ = false;
    scheduleGlance(ms);
  }
  if (s.action == 1) bounce(ms);
  if (s.action == 2) say(L_FED, false, ms);
  if (s.sound) sound_ = s.sound - 1;
}

void Pet::quirk(uint32_t ms) {
  float side = rand01() < 0.5f ? -1 : 1;
  switch (mood_) {
    case CALM:
    case NEWBORN: {
      // A sideways, knowing look.
      Step s[] = {{E_SLY, true, side * 0.85f, 0.55f, 1500, 0}};
      play(s, 1, ms);
      break;
    }
    case SATIATED: {
      // A contented squeeze.
      Step s[] = {{E_TICKLED, false, 0, 0, 900, 0}};
      play(s, 1, ms);
      break;
    }
    case ACTIVE: {
      // A pop of surprise at its own bubbles.
      Step s[] = {{E_SURPRISED, true, side * 0.4f, -0.6f, 700, 0}};
      play(s, 1, ms);
      break;
    }
    case PECKISH: {
      // Glances down, a little sorry for itself.
      Step s[] = {{E_SAD, true, 0, 0.7f, 1500, 0}};
      play(s, 1, ms);
      break;
    }
    case HUNGRY: {
      // Looks down at the "Nutri" button, then back at you.
      Step s[] = {{E_WORRIED, true, 0, 1, 1100, 0, SND_HUNGRY + 1}, {E_SAD, true, 0, 0, 900, 0}};
      play(s, 2, ms);
      break;
    }
    case STARVING: {
      // Nods off and jerks awake.
      Step s[] = {{E_SLEEP, false, 0, 0, 1300, 0, SND_YAWN + 1}, {E_SURPRISED, false, 0, 0, 350, 0}};
      play(s, 2, ms);
      break;
    }
    default:
      break;
  }
}

void Pet::update(uint32_t ms) {
  float dt = (int32_t)(ms - now_) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 0.1f) dt = 0.1f;
  now_ = ms;

  // Reaction sequence.
  if (steps_ > 0 && due(ms, stepStart_ + seq_[stepIndex_].ms)) {
    if (++stepIndex_ >= steps_) {
      steps_ = 0;
      if (gazeHeld_) {
        gazeHeld_ = false;
        scheduleGlance(ms);
      }
    } else {
      startStep(ms);
    }
  }

  // Expression and colours.
  ExprName e = currentExpr();
  if (e != expr_) setExpression(e, ms);
  float k = easeOutQuint(clamp01((float)(ms - exprStart_) / EXPR_MS));
  cur_ = {mix(from_.lid, to_.lid, k),          mix(from_.tilt, to_.tilt, k),         mix(from_.low, to_.low, k),
          mix(from_.happy, to_.happy, k),      mix(from_.eyeScale, to_.eyeScale, k), mix(from_.pupil, to_.pupil, k)};
  alive_ = mix(aliveFrom_, aliveTo_, easeInOutQuad(clamp01((float)(ms - exprStart_) / ALIVE_MS)));
  Palette p = paletteFor(mood_, asleep_, expr_);
  if (p.eye != eyeTo_ || p.pupil != pupilTo_) {
    eyeFrom_ = eye_;
    pupilFrom_ = pupil_;
    eyeTo_ = p.eye;
    pupilTo_ = p.pupil;
    colorStart_ = ms;
  }
  float kc = easeInOutQuad(clamp01((float)(ms - colorStart_) / COLOR_MS));
  eye_ = lerpColor(eyeFrom_, eyeTo_, kc);
  pupil_ = lerpColor(pupilFrom_, pupilTo_, kc);

  // Breathing: a slow float.
  if (live_.breathSeconds > 0) breathPhase_ = fmodf(breathPhase_ + dt / live_.breathSeconds, 1);
  breath_ = 0.5f - 0.5f * cosf(2 * (float)M_PI * breathPhase_);

  // Blinking: fast close, softer open; sometimes twice. Not while the eyes are shut.
  bool shut = to_.lid >= 1;
  if (blinkMode_) {
    uint32_t el = ms - blinkStart_;
    if (blinkMode_ == 2 && el >= 295) el -= 295;  // second blink after an 80 ms pause
    else if (blinkMode_ == 2 && el >= 215) el = 215;
    if (el < 65) blink_ = (el / 65.0f) * (el / 65.0f);
    else if (el < 215) blink_ = 1 - easeOutCubic((el - 65) / 150.0f);
    else blink_ = 0;
    if (ms - blinkStart_ >= (blinkMode_ == 2 ? 510u : 215u)) {
      blinkMode_ = 0;
      blink_ = 0;
    }
  } else if (shut) {
    scheduleBlink(ms);
  } else if (due(ms, nextBlink_)) {
    blinkMode_ = rand01() < 0.2f ? 2 : 1;
    blinkStart_ = ms;
    scheduleBlink(ms);
  }

  // Bounce: up and stretched, then a springy settle that briefly squashes.
  if (bounceMode_ == 1 && ms - bounceStart_ >= 110) {
    bounceMode_ = 2;
    bounce_ = Spring();
    bounce_.x = bounceAmp_;
    // Little hops settle quicker than a full bounce.
    bounce_.damping = bounceAmp_ < 1 ? 8 : 6;
    bounce_.stiffness = bounceAmp_ < 1 ? 240 : 190;
    bounce_.mass = 1;
  }
  // Little hops on the spot when lively (an active, bubbling starter).
  if (live_.hopSeconds > 0 && to_.lid < 1 && due(ms, nextHop_)) {
    if (bounceMode_ == 0) bounce(ms, 0.35f);
    nextHop_ = ms + (uint32_t)(live_.hopSeconds * 1000 * (0.7f + rand01() * 0.6f));
  }
  if (bounceMode_ == 2) {
    bounce_.step(dt);
    if (fabsf(bounce_.x) < 0.002f && fabsf(bounce_.v) < 0.02f) bounceMode_ = 0;
  }

  // Gaze.
  if (leftPending_ && due(ms, leftAt_)) {
    lookLX_.target = leftTX_;
    lookLY_.target = leftTY_;
    leftPending_ = false;
  }
  press_.step(dt);
  lookRX_.step(dt);
  lookRY_.step(dt);
  lookLX_.step(dt);
  lookLY_.step(dt);
  if (live_.glanceSeconds > 0 && !gazeHeld_ && due(ms, nextGlance_)) {
    if (!touching_) {
      bool atYou = rand01() < live_.lookAtYou;
      float r = live_.glanceRange * GLANCE_NARROWER;
      float x = atYou ? 0 : (rand01() * 2 - 1) * r;
      float y = atYou ? 0 : (rand01() * 1.6f - 0.8f) * r;
      look(x, y, SACCADE.damping, SACCADE.stiffness, SACCADE.mass, ms);
    }
    scheduleGlance(ms);
  }

  // Idle quirks and chatter.
  if (hasState_ && !asleep_ && mood_ != DORMANT && due(ms, nextQuirk_)) {
    if (steps_ == 0) quirk(ms);
    nextQuirk_ = ms + 9000 + (uint32_t)(rand01() * 7000);
  }
  if (line_ && lineUntil_ && due(ms, lineUntil_)) {
    line_ = nullptr;
    lineId_++;
    lineUntil_ = 0;
  }
  if (hasState_ && due(ms, nextLine_)) {
    if (!persistentMood(mood_) && !asleep_) say((LineKey)mood_, false, ms);
    nextLine_ = ms + CHAT_MS;
  }

  // Particles.
  bubbles_ = mix(bubblesFrom_, bubblesTo_, easeInOutQuad(clamp01((float)(ms - bubblesStart_) / 800)));
  // Zzz while asleep, and during a nap after a downward swipe.
  float zzzTarget = asleep_ || mood_ == DORMANT || !due(ms, napUntil_) ? 1 : 0;
  if (zzzTarget != zzzTo_) {
    zzzFrom_ = zzz_;
    zzzTo_ = zzzTarget;
    zzzStart_ = ms;
  }
  zzz_ = mix(zzzFrom_, zzzTo_, easeInOutQuad(clamp01((float)(ms - zzzStart_) / 600)));
  // The last heart starts at 0.74 of the meal and needs 0.45 more: let it fly out.
  if (feeding_ && ms - feedStart_ > FEED_MS * 5 / 4) feeding_ = false;
  if (loving_ && ms - loveStart_ > LOVE_MS) loving_ = false;
}

EyeInput Pet::readEye(int side) const {
  float t = (now_ - t0_) / 1000.0f;
  float lx = (side < 0 ? lookLX_.x : lookRX_.x) + sinf(t * 0.9f + side * 1.7f) * 0.035f * alive_;
  float ly = (side < 0 ? lookLY_.x : lookRY_.x) + cosf(t * 0.7f + side) * 0.03f * alive_;
  float b = bounceMode_ == 1 ? bounceFrom_ + (bounceAmp_ - bounceFrom_) * easeOutQuad(clamp01((now_ - bounceStart_) / 110.0f)) : bounceMode_ == 2 ? bounce_.x : 0;
  EyeInput e;
  e.side = side;
  e.lid = cur_.lid;
  e.tilt = cur_.tilt;
  e.low = cur_.low;
  e.happy = cur_.happy;
  e.eyeScale = cur_.eyeScale;
  e.pupil = cur_.pupil;
  e.lookX = lx;
  e.lookY = ly;
  e.blink = blink_;
  e.dy = breath_ * 3 - b * 10 + press_.x * 4;
  e.stretch = 1 + breath_ * 0.012f - blink_ * 0.07f + b * 0.09f - press_.x * 0.08f;
  return e;
}

void Pet::frame(StageFrame &f) const {
  f.eyes[0] = eyeShape(readEye(-1));
  f.eyes[1] = eyeShape(readEye(1));
  f.eye = eye_;
  f.pupil = pupil_;

  const float W = STAGE_W, H = STAGE_H, eyesY = STAGE_EYES_Y;
  float clock = (now_ - t0_) / 1000.0f;
  f.nDots = f.nHearts = f.nZeds = 0;

  // Fermentation bubbles rising from the bottom.
  if (bubbles_ > 0.003f) {
    for (const BubbleDef &b : BUBBLES) {
      float t = fmodf(clock / (period_ / b.speed) + b.phase, 1);
      float sway = sinf((clock + b.phase * 10) * 1.6f) * 6;
      float fade = fminf(1, t * 5) * fminf(1, (1 - t) * 4);
      f.dots[f.nDots++] = {b.x * W + sway, H - t * H * 0.95f, b.r * (0.9f + t * 0.7f), 0.32f * fade * bubbles_};
    }
  }
  // Sleep Zzz.
  if (zzz_ > 0.003f) {
    for (int i = 0; i < 3; i++) {
      float t = fmodf(clock / 3.6f + i / 3.0f, 1);
      f.zeds[f.nZeds++] = {W * 0.66f + t * 34 + i * 4, eyesY - 50 - t * 70, 16 + t * 14, zzz_ * fminf(1, t * 4) * (1 - t)};
    }
  }
  // The meal: flour falling in, then hearts.
  if (feeding_) {
    float feed = (float)(now_ - feedStart_) / FEED_MS;  // runs to 1.25 so every heart finishes
    if (feed > 0 && feed < 0.45f) {
      for (int i = 0; i < 10 && f.nDots < MAX_DOTS; i++) {
        float t = clamp01((feed - (i % 5) * 0.03f) / 0.32f);
        f.dots[f.nDots++] = {FLOUR[i] * W + sinf(i * 2.3f) * 8, -10 + t * t * (eyesY + 10), 2.5f + (i % 3), 1 - t * 0.6f};
      }
    }
    for (const HeartDef &h : HEARTS) {
      float t = clamp01((feed - 0.5f - h.delay) / 0.45f);
      if (t <= 0 || t >= 1) continue;
      f.hearts[f.nHearts++] = {h.x * W, eyesY - 30 - t * 110, h.size * (0.6f + t * 0.6f), fminf(1, t * 5) * (1 - t)};
    }
  }
  // A cuddle: the same hearts, right away.
  if (loving_ && f.nHearts == 0) {
    float love = (float)(now_ - loveStart_) / FEED_MS;
    for (const HeartDef &h : HEARTS) {
      float t = clamp01((love - h.delay) / 0.45f);
      if (t <= 0 || t >= 1) continue;
      f.hearts[f.nHearts++] = {h.x * W, eyesY - 30 - t * 110, h.size * (0.6f + t * 0.6f), fminf(1, t * 5) * (1 - t)};
    }
  }
}

// ─── Input ───────────────────────────────────────────────────────────────────
//
// Gestures instead of a gaze that follows the finger: a tap giggles, a double
// tap cuddles (hearts), swiping down lulls it to sleep for a moment, swiping
// up startles it awake, a sideways swipe makes it glance that way. Poking it
// again and again makes it cross. At night any gesture wakes it up grumpy.

void Pet::touchDown(float x, float y, uint32_t ms) {
  touching_ = true;
  touchX_ = x;
  touchY_ = y;
  touchStart_ = ms;
  // Instant feedback: the eyes give under the finger like jelly, before the
  // gesture is known; they spring back on release.
  press_.target = 1;
}

void Pet::touchUp(float x, float y, uint32_t ms) {
  touching_ = false;
  press_.target = 0;
  float dx = x - touchX_, dy = y - touchY_, dist = hypotf(dx, dy);
  uint32_t held = ms - touchStart_;
  if (dist >= SWIPE_PX && held <= SWIPE_MS) swipe(dx, dy, ms);
  else if (dist < TAP_PX && held < TAP_MS) tap(ms);
}

// True when the gesture is spent on waking it up or keeping it cross.
bool Pet::grumpyFirst(uint32_t ms) {
  if (asleep_) {
    Step s[] = {{E_GRUMPY, false, 0, 0, 1800, 1, SND_YAWN + 1}};
    play(s, 1, ms);
    say(L_WOKEN, false, ms);
    return true;
  }
  if (steps_ > 0 && seq_[stepIndex_].expr == E_ANNOYED) {
    // Still cross: more poking only keeps it cross, with a huff.
    Step s[] = {{E_ANNOYED, false, 0, 0, 1600, 1, SND_ANNOYED + 1}};
    play(s, 1, ms);
    return true;
  }
  return false;
}

void Pet::tap(uint32_t ms) {
  if (mood_ == DORMANT || feeding_ || grumpyFirst(ms)) return;
  int recent = 0;
  for (int i = 0; i < 3; i++) taps_[i] = taps_[i + 1];
  taps_[3] = ms;
  for (uint32_t t : taps_) recent += t && ms - t < ANNOY_WINDOW_MS;
  if (recent >= 4) {
    memset(taps_, 0, sizeof(taps_));
    lastTap_ = 0;
    annoy(ms);
    return;
  }
  if (lastTap_ && ms - lastTap_ < DOUBLE_TAP_MS) {
    // A cuddle: a happy hop, then content eyes while hearts rise.
    lastTap_ = 0;
    loving_ = true;
    loveStart_ = ms;
    Step s[] = {{E_TICKLED, false, 0, 0, 360, 1, SND_LOVE + 1}, {E_CONTENT, false, 0, 0, 1600, 0}};
    play(s, 2, ms);
    return;
  }
  lastTap_ = ms;
  giggle(ms);
}

void Pet::giggle(uint32_t ms) {
  // Laughing eyes and three quick hops.
  Step s[] = {{E_TICKLED, false, 0, 0, 280, 1, SND_GIGGLE + 1}, {E_TICKLED, false, 0, 0, 280, 1}, {E_TICKLED, false, 0, 0, 640, 1}};
  play(s, 3, ms);
  say(L_TICKLED, false, ms);
}

void Pet::annoy(uint32_t ms) {
  Step s[] = {{E_ANNOYED, false, 0, 0, 1900, 0, SND_ANNOYED + 1}};
  play(s, 1, ms);
  say(L_ANNOYED, false, ms);
}

void Pet::grumpy(uint32_t ms) {
  Step s[] = {{E_GRUMPY, false, 0, 0, 1800, 1, SND_YAWN + 1}};
  play(s, 1, ms);
  say(L_WOKEN, false, ms);
}

void Pet::quirkNow(uint32_t ms) {
  quirk(ms);
}

void Pet::swipe(float dx, float dy, uint32_t ms) {
  // Up is the mute switch, whatever its mood.
  if (fabsf(dy) > fabsf(dx) && dy < 0) {
    muteToggle_ = true;
    return;
  }
  if (mood_ == DORMANT || feeding_ || grumpyFirst(ms)) return;
  if (fabsf(dy) > fabsf(dx) && dy > 0) {
    // Lids pulled down: drowsy, a short nap with Zzz, then open again.
    Step s[] = {{E_TIRED, false, 0, 0.3f, 450, 0, SND_YAWN + 1}, {E_SLEEP, false, 0, 0, 1800, 0}};
    play(s, 2, ms);
    napUntil_ = ms + 2250;
  } else {
    // Sideways, like a wave: it glances where the hand went, then turns to
    // you with a hop and says hello.
    float side = dx > 0 ? 1 : -1;
    Step s[] = {{E_SLY, true, side * 0.9f, 0.25f, 450, 0}, {E_EXCITED, true, 0, 0, 900, 1, SND_HELLO + 1}};
    play(s, 2, ms);
  }
}

void Pet::soundToggled(bool muted, uint32_t ms) {
  if (muted) {
    // "Shh": eyes squeezed shut for a moment, in silence.
    Step s[] = {{E_TIRED, false, 0, 0.2f, 350, 0}, {E_SLEEP, false, 0, 0, 450, 0}};
    play(s, 2, ms);
  } else {
    Step s[] = {{E_EXCITED, false, 0, 0, 900, 1, SND_HELLO + 1}};
    play(s, 1, ms);
  }
}

void Pet::shake(uint32_t ms) {
  // A shake wins over quirks and other reactions, not over a meal.
  if (mood_ == DORMANT || feeding_) return;
  if (asleep_) {
    Step s[] = {{E_GRUMPY, false, 0, 0, 1800, 0, SND_YAWN + 1}};
    play(s, 1, ms);
    say(L_WOKEN, false, ms);
    return;
  }
  // A flash of surprise, then properly cross: "you shook me!"
  Step s[] = {{E_SURPRISED, false, 0, 0, 250, 1, SND_STARTLED + 1}, {E_ANNOYED, false, 0, 0, 2000, 0}};
  play(s, 2, ms);
}

void Pet::celebrate(uint32_t ms) {
  feeding_ = true;
  feedStart_ = ms;
  Step s[] = {
      // Looks up at the flour falling in…
      {E_SURPRISED, true, 0, -1, 900, 0},
      // …munches it…
      {E_EATING, true, 0, 0.2f, 300, 1, SND_MUNCH + 1},
      {E_EATING, true, 0, 0.2f, 300, 1},
      {E_EATING, true, 0, 0.2f, 400, 1},
      // …and beams.
      {E_TICKLED, false, 0, 0, 1600, 2, SND_LOVE + 1},
  };
  play(s, 5, ms);
}
