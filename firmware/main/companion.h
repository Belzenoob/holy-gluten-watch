#pragma once
#include <stddef.h>
#include <stdint.h>
#include "face.h"

// Companions on the watch: the starter's creature, drawn as polygons sampled
// from the Holy Gluten app's drawings and packed into one binary file. The
// pack lives in the flash partition `cmp`; without it the watch shows Madre. This
// reader maps one companion's blob and describes it with small structs that
// point into the mapped memory: the keys are never copied into RAM.
// Pure C++ apart from the partition access (ESP_PLATFORM), so the stage and
// the pet build on the Mac, where the pack is a file read into memory.

static constexpr int COMP_STATES = 416;  // mood 8 × asleep 2 × expression 13 × atPeak 2
static constexpr int COMP_MAX_PARTS = 24;
static constexpr int COMP_MAX_SUBS = 128;
// Points of a companion's parts, with the closing point of each closed sub-path.
static constexpr int COMP_MAX_POINTS = 1024;
// Points of its stroked parts (they need extra per-segment numbers).
static constexpr int COMP_MAX_STROKE_POINTS = 512;

enum PartPaint : uint8_t { PAINT_EYE, PAINT_PUPIL, PAINT_ACCENT, PAINT_ACCENT_SOFT, PAINT_BG };

// Delta sets: how a point moves per unit of each input.
enum DeltaSet : uint8_t { SET_GAZE_X, SET_GAZE_Y, SET_BREATH, SET_BOUNCE, SETS };

struct CompSub {
  uint16_t n;
  bool closed;
  uint8_t eye;  // lid-clipped parts: 1 = cut by the left eye's lids, 2 = the right's, 0 = not cut
};

struct CompPart {
  uint8_t layer;  // 0 behind the eyes, 1 in front
  uint8_t paint;  // PartPaint
  uint8_t nSub, mask;  // mask: bit s = delta set s is stored
  uint8_t nSets;
  bool clip;      // exported whole: the stage cuts each sub-path with its eye's lids, as the app does a pupil
  bool gazeDisc;  // the gaze is limited to the unit disc before it moves the part (as a pupil's)
  float stroke;  // 0 = even-odd fill; else line width, design units
  uint16_t nKeys, firstSub, nPoints;
  const uint8_t *map;   // COMP_STATES × u16 LE; nullptr when there is a single key
  const uint8_t *keys;  // nKeys blocks of keySize bytes
  uint32_t keySize;
};

struct Companion {
  char id[13];
  bool ownEyes;    // it draws its eyes itself, as parts
  bool nocturnal;  // asleep in the day
  EyeBuild eyes;
  Expression expr[E_COUNT];
  uint32_t palette[PAL_ROWS][4];  // 0xRRGGBB: eye, pupil, accent, accentSoft
  int nParts, nSubs, nPoints, nStrokePoints;
  CompPart parts[COMP_MAX_PARTS];
  CompSub subs[COMP_MAX_SUBS];
};

// Index into a part's state map for a state of the pet.
static inline int companionState(Mood mood, bool asleep, ExprName expr, bool atPeak) {
  return (((int)mood * 2 + (asleep ? 1 : 0)) * E_COUNT + (int)expr) * 2 + (atPeak ? 1 : 0);
}

// ─── Reading the mapped blob ────────────────────────────────────────────────
// Little endian, 4-byte aligned in the pack; read byte-wise so alignment never matters.

static inline uint16_t rdU16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline int16_t rdI16(const uint8_t *p) {
  return (int16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rdU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
float rdF32(const uint8_t *p);

// One key of a part: opacity, points (x, y i16 in 1/16 design unit) and the delta sets present.
struct PartKey {
  uint8_t opacity;
  const uint8_t *pts;       // nPoints × (x, y)
  const uint8_t *delta[SETS];  // nPoints × (dx, dy) i8, or nullptr when the set is absent
  float scale[SETS];        // displacement at input 1 = i8 × scale / 127, design units
};
void companionPartKey(const CompPart &p, int state, PartKey &out);

// ─── The pack ───────────────────────────────────────────────────────────────

// Device: finds the `cmp` data partition and maps its directory. Anywhere:
// companionUsePack() reads the pack from memory instead (the Mac tests, which
// load the file; the memory must outlive its use). False when there is no
// valid pack.
bool companionOpenPack();
void companionUsePack(const uint8_t *data, size_t size);
// Selects a companion by id: its blob is mapped and parsed. Null for Madre
// ("" or "madre"), for an unknown id, a missing or damaged pack, or a blob
// that does not fit the stage's limits; the caller then shows Madre.
const Companion *companionSelect(const char *id);
const Companion *companionCurrent();
// How many companions the pack holds, and the id of the n-th (tests).
int companionCount();
const char *companionIdAt(int n);
// What the last failed select complained about (diagnostics).
const char *companionError();
