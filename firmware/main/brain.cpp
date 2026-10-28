#include "brain.h"
#include <math.h>
#include <stdio.h>
#include <algorithm>

static constexpr int64_t HOUR = 3600;
static constexpr int64_t DAY = 24 * HOUR;
// An interval counts as on time with a little slack over the reminder.
static constexpr float ON_TIME_HOURS = REMINDER_DAYS * 24 + 12;
static constexpr float DORMANT_HOURS = 9 * 24;
static constexpr int NIGHT_FROM = 23;
static constexpr int NIGHT_UNTIL = 7;
static constexpr int DAY_FROM = 11;
static constexpr int DAY_UNTIL = 19;
// Reminders never ring between these hours; they move to the evening before.
static constexpr int QUIET_FROM = 21;
static constexpr int QUIET_UNTIL = 8;
static constexpr int EVENING = 20;

static float clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int localHour(int64_t t, int tz) {
  int64_t local = t + (int64_t)tz * 60;
  return (int)((local - floorDiv(local, DAY) * DAY) / HOUR);
}

int64_t reminderAt(int64_t lastRefresh, int tz) {
  int64_t dueLocal = lastRefresh + REMINDER_DAYS * DAY + (int64_t)tz * 60;
  int64_t dayStart = floorDiv(dueLocal, DAY) * DAY;
  int h = (int)((dueLocal - dayStart) / HOUR);
  if (h >= QUIET_FROM) dueLocal = dayStart + EVENING * HOUR;
  else if (h < QUIET_UNTIL) dueLocal = dayStart - DAY + EVENING * HOUR;
  return dueLocal - (int64_t)tz * 60;
}

Mood moodFor(float hours, int64_t lastRefresh, int tz) {
  if (hours < 2) return SATIATED;
  if (hours < 12) return ACTIVE;
  if (hours < 72) return CALM;
  if (hours < 96) return PECKISH;
  if (hours >= DORMANT_HOURS) return DORMANT;
  if (lastRefresh + (int64_t)(hours * HOUR) >= reminderAt(lastRefresh, tz)) return STARVING;
  return HUNGRY;
}

static Stage stageFor(int ageDays) {
  if (ageDays < 7) return NEONATA;
  if (ageDays < 30) return GIOVANE;
  if (ageDays < 180) return ADULTA;
  return VETERANA;
}

PetState petState(const int64_t *input, size_t n, int64_t now, int tz, bool nocturnal) {
  PetState s;
  int hourOfDay = localHour(now, tz);
  bool night = nocturnal ? hourOfDay >= DAY_FROM && hourOfDay < DAY_UNTIL : hourOfDay >= NIGHT_FROM || hourOfDay < NIGHT_UNTIL;
  if (n == 0) return s;

  int64_t times[64];
  n = std::min(n, sizeof(times) / sizeof(times[0]));
  std::copy(input, input + n, times);
  std::sort(times, times + n, [](int64_t a, int64_t b) { return a > b; });

  int64_t last = times[0];
  float hours = std::max(0.0f, (float)(now - last) / HOUR);
  Mood mood = moodFor(hours, last, tz);
  bool overdue = mood == STARVING || mood == DORMANT;

  float intervals[7];
  size_t count = std::min(n - 1, (size_t)7);
  for (size_t i = 0; i < count; i++) intervals[i] = (float)(times[i] - times[i + 1]) / HOUR;

  int streak = 0;
  if (!overdue) {
    for (size_t i = 0; i < count && intervals[i] <= ON_TIME_HOURS; i++) streak++;
  }
  size_t onTime = 0;
  for (size_t i = 0; i < count; i++) onTime += intervals[i] <= ON_TIME_HOURS;
  float regularity = count ? (float)onTime / count : 0.6f;
  float neglect = overdue ? clamp01(1 - (hours - REMINDER_DAYS * 24) / (REMINDER_DAYS * 24)) : 1;
  int ageDays = (int)floorDiv(now - times[n - 1], DAY);

  s.mood = mood;
  s.asleep = night && (mood == SATIATED || mood == ACTIVE || mood == CALM || mood == PECKISH);
  s.fed = true;
  s.hours = hours;
  s.satiety = clamp01(1 - hours / (REMINDER_DAYS * 24));
  s.vitality = clamp01(regularity * neglect);
  s.streak = streak;
  s.ageDays = ageDays;
  s.stage = stageFor(ageDays);
  s.atPeak = hours >= 3 && hours <= 8;
  return s;
}

int segments(float value, int total) {
  if (value <= 0) return 0;
  int k = (int)ceilf(value * total);
  return k < 1 ? 1 : k > total ? total : k;
}

static const char *const NEWBORN_L[] = {"Ciao! Nutrimi per cominciare.", "Sono pronta a crescere: un rinfresco?"};
static const char *const SATIATED_L[] = {"Gnam! Grazie.", "Che bel rinfresco.", "Mi sento piena di energia."};
static const char *const ACTIVE_L[] = {"Sto facendo le bolle!", "Guarda come cresco.", "Sento il profumo di acido buono."};
static const char *const CALM_L[] = {"Tutto tranquillo qui dentro.", "Ci vediamo al prossimo rinfresco.", "Riposo e fermento piano."};
static const char *const PECKISH_L[] = {"Tra un po’ avrò fame…", "Hai pensato al rinfresco?", "Comincio a sgonfiarmi."};
static const char *const HUNGRY_L[] = {"Ho fame!", "Un po’ di farina, per favore?", "Domani sarebbe il giorno giusto."};
static const char *const STARVING_L[] = {"Mi stai dimenticando…", "Sento l’acido salire.", "Farina… acqua… ti prego."};
static const char *const DORMANT_L[] = {"Zzz… svegliami con un rinfresco.", "Sono in letargo. Un rinfresco e torno."};
static const char *const ASLEEP_L[] = {"Zzz…", "Fermento anche di notte."};
static const char *const TICKLED_L[] = {"Ihih!", "Soffice, vero?", "Fa il solletico!"};
static const char *const ANNOYED_L[] = {"Ehi! Così mi sgonfi.", "Basta, mi fai fermentare troppo!", "Lasciami lievitare in pace."};
static const char *const FED_L[] = {"Gnam! Grazie.", "Farina fresca, che bontà.", "Ora sì che si ragiona."};
static const char *const WOKEN_L[] = {"Mmh… stavo dormendo.", "È notte, lasciami fermentare."};

struct LineList {
  const char *const *items;
  uint8_t count;
};
#define LIST(a) {a, sizeof(a) / sizeof(a[0])}
static const LineList LINES[L_COUNT] = {
    LIST(NEWBORN_L), LIST(SATIATED_L), LIST(ACTIVE_L), LIST(CALM_L), LIST(PECKISH_L), LIST(HUNGRY_L),  LIST(STARVING_L),
    LIST(DORMANT_L), LIST(ASLEEP_L),   LIST(TICKLED_L), LIST(ANNOYED_L), LIST(FED_L),  LIST(WOKEN_L),
};

const char *petLine(LineKey key, uint32_t seed) {
  const LineList &l = LINES[key < L_COUNT ? key : L_CALM];
  return l.items[seed % l.count];
}

void statusText(const PetState &s, const char *name, char *out, size_t size) {
  if (!s.fed) {
    snprintf(out, size, "%s aspetta il primo rinfresco", name);
    return;
  }
  char ago[40];
  float h = s.hours;
  if (h < 1) snprintf(ago, sizeof(ago), "meno di un’ora fa");
  else if (h < 24) snprintf(ago, sizeof(ago), "%d %s fa", (int)h, (int)h == 1 ? "ora" : "ore");
  else snprintf(ago, sizeof(ago), "%d %s fa", (int)(h / 24), (int)(h / 24) == 1 ? "giorno" : "giorni");
  if (s.atPeak) {
    snprintf(out, size, "Al picco: è il momento migliore per usarla · rinfrescata %s", ago);
    return;
  }
  switch (s.mood) {
    case STARVING: snprintf(out, size, "Ha bisogno di un rinfresco · ultimo %s", ago); break;
    case DORMANT: snprintf(out, size, "In letargo · ultimo rinfresco %s", ago); break;
    case HUNGRY: snprintf(out, size, "Ha fame · rinfrescata %s", ago); break;
    default: snprintf(out, size, "Rinfrescata %s", ago); break;
  }
}

const char *stageLabel(Stage s) {
  static const char *const LABELS[] = {"neonata", "giovane", "adulta", "veterana"};
  return LABELS[s];
}
