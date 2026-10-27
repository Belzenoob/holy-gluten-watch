#pragma once
#include <stdint.h>
#include <stddef.h>

// The starter pet's "brain": a 1:1 port of src/features/starter/pet/brain.ts
// and of reminderAt() in src/domain/starter.ts. Pure C++, no Arduino, so the
// host tests in firmware/test/ run it on the Mac. Times are unix seconds;
// `tz` is the phone's offset in minutes east of UTC.

enum Mood : uint8_t { NEWBORN, SATIATED, ACTIVE, CALM, PECKISH, HUNGRY, STARVING, DORMANT };
enum Stage : uint8_t { NEONATA, GIOVANE, ADULTA, VETERANA };

struct PetState {
  Mood mood = NEWBORN;
  bool asleep = false;
  bool fed = false;     // false = never fed (hours undefined)
  float hours = 0;      // since the last refresh
  float satiety = 0;    // 1 right after feeding, 0 at the reminder
  float vitality = 0.5f;
  int streak = 0;
  int ageDays = 0;
  Stage stage = NEONATA;
  bool atPeak = false;
};

// Line keys: the moods, then the reactions.
enum LineKey : uint8_t {
  L_NEWBORN, L_SATIATED, L_ACTIVE, L_CALM, L_PECKISH, L_HUNGRY, L_STARVING, L_DORMANT,
  L_ASLEEP, L_TICKLED, L_ANNOYED, L_FED, L_WOKEN, L_COUNT
};

static constexpr int REMINDER_DAYS = 5;

int64_t reminderAt(int64_t lastRefresh, int tz);
Mood moodFor(float hours, int64_t lastRefresh, int tz);
// `times`: refresh times, any order; `n` of them.
PetState petState(const int64_t *times, size_t n, int64_t now, int tz);
int segments(float value, int total = 5);
const char *petLine(LineKey key, uint32_t seed);
// The status sentence under the pet, UTF-8, into `out`.
void statusText(const PetState &s, const char *name, char *out, size_t size);
const char *stageLabel(Stage s);
