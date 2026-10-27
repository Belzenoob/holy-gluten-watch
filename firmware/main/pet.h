#pragma once
#include <stdint.h>
#include "brain.h"
#include "face.h"
#include "stage.h"
#include "sounds.h"

// The pet's behaviour, a port of PetEyes.tsx, PetParticles.tsx and the
// reactions of PetScreen.tsx: expressions that ease in, springy glances,
// blinks, breathing, bounces, idle quirks, reactions and lines. Pure C++
// driven by a millisecond clock, so it runs the same on the Mac.

struct Spring {
  float x = 0, v = 0, target = 0;
  float damping = 15, stiffness = 320, mass = 0.7f;
  void step(float dt);
};

class Pet {
 public:
  void begin(uint32_t ms, uint32_t seed);
  // From the brain: call on every refresh; only changes do something.
  void setState(Mood mood, bool asleep, uint32_t ms);
  void update(uint32_t ms);
  void frame(StageFrame &f) const;

  // Touch on the stage, in stage pixels: taps and swipes become reactions.
  void touchDown(float x, float y, uint32_t ms);
  void touchUp(float x, float y, uint32_t ms);
  // A sudden shake: startled, then cross.
  void shake(uint32_t ms);
  // The meal: flour falls, it munches, hearts rise.
  void celebrate(uint32_t ms);
  // Reactions on request (the app's test panel), same as the gestures'.
  void giggle(uint32_t ms);
  void annoy(uint32_t ms);
  void grumpy(uint32_t ms);
  void quirkNow(uint32_t ms);

  // Line under the eyes, or nullptr; `lineId` changes with every new line.
  const char *line() const { return line_; }
  uint32_t lineId() const { return lineId_; }
  bool busy() const { return steps_ > 0; }
  // A swipe up asks to switch the sound on or off; reading it clears it.
  bool takeMuteToggle() {
    bool t = muteToggle_;
    muteToggle_ = false;
    return t;
  }
  // The face that answers the mute switch: eyes shut a moment when muted, a hop and hello when not.
  void soundToggled(bool muted, uint32_t ms);
  // The sound a reaction asks for (-1 = none); reading it clears it.
  int takeSound() {
    int s = sound_;
    sound_ = -1;
    return s;
  }

 private:
  struct Step {
    ExprName expr;
    bool hasGaze;
    float gx, gy;
    uint16_t ms;
    uint8_t action;  // 0 none, 1 bounce, 2 say "fed"
    uint8_t sound;   // 0 none, else Sound + 1, played as the step starts
  };

  uint32_t rand32();
  float rand01() { return (rand32() >> 8) * (1.0f / 16777216.0f); }

  void play(const Step *steps, int n, uint32_t ms);
  void tap(uint32_t ms);
  void swipe(float dx, float dy, uint32_t ms);
  bool grumpyFirst(uint32_t ms);
  void startStep(uint32_t ms);
  void say(LineKey key, bool persistent, uint32_t ms);
  void quirk(uint32_t ms);
  void look(float x, float y, float damping, float stiffness, float mass, uint32_t ms);
  void setExpression(ExprName e, uint32_t ms);
  void bounce(uint32_t ms, float amp = 1);
  void scheduleGlance(uint32_t ms);
  void scheduleBlink(uint32_t ms);
  EyeInput readEye(int side) const;
  ExprName currentExpr() const;

  uint32_t rng_ = 1;
  uint32_t t0_ = 0, now_ = 0;
  Mood mood_ = NEWBORN;
  bool asleep_ = false, hasState_ = false;
  Liveliness live_{};

  // Reaction sequence.
  Step seq_[6];
  int steps_ = 0, stepIndex_ = 0;
  uint32_t stepStart_ = 0;

  // Expression tween.
  ExprName expr_ = E_NEUTRAL;
  Expression from_{}, to_{}, cur_{};
  uint32_t exprStart_ = 0;
  float aliveFrom_ = 1, aliveTo_ = 1, alive_ = 1;

  // Colours.
  uint32_t eyeFrom_ = 0, eyeTo_ = 0, pupilFrom_ = 0, pupilTo_ = 0, eye_ = 0, pupil_ = 0;
  uint32_t colorStart_ = 0;

  // Motion.
  Spring lookRX_, lookRY_, lookLX_, lookLY_;
  Spring press_{0, 0, 0, 14, 420, 0.5f};  // squash under the finger
  float leftTX_ = 0, leftTY_ = 0;
  uint32_t leftAt_ = 0;
  bool leftPending_ = false;
  float breathPhase_ = 0, breath_ = 0;
  int bounceMode_ = 0;  // 0 rest, 1 rising, 2 spring
  uint32_t bounceStart_ = 0;
  float bounceFrom_ = 0, bounceAmp_ = 1;
  uint32_t nextHop_ = 0;
  Spring bounce_;
  int blinkMode_ = 0;  // 0 idle, 1 single, 2 double
  uint32_t blinkStart_ = 0, nextBlink_ = 0;
  float blink_ = 0;
  uint32_t nextGlance_ = 0, nextQuirk_ = 0, nextLine_ = 0;
  bool touching_ = false, gazeHeld_ = false;
  float touchX_ = 0, touchY_ = 0;
  uint32_t touchStart_ = 0;
  uint32_t taps_[4] = {0, 0, 0, 0};
  uint32_t lastTap_ = 0, napUntil_ = 0, loveStart_ = 0;
  bool loving_ = false;

  int sound_ = -1;
  bool muteToggle_ = false;

  // Line.
  const char *line_ = nullptr;
  uint32_t lineId_ = 0, lineUntil_ = 0;

  // Particles.
  float bubbles_ = 0, bubblesFrom_ = 0, bubblesTo_ = 0, period_ = 6;
  uint32_t bubblesStart_ = 0;
  float zzz_ = 0, zzzFrom_ = 0, zzzTo_ = 0;
  uint32_t zzzStart_ = 0;
  uint32_t feedStart_ = 0;
  bool feeding_ = false;
};
