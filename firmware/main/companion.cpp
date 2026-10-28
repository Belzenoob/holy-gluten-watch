#include "companion.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_partition.h"
#endif

// Blob layout (version 2), all little endian, offsets from the blob's start:
//   "MCMP" | version u16 | flags u16 (bit0 ownEyes, bit1 nocturnal, bits 2-4 voice)
//   eyes 7 × f32 | expr 13 × 6 × f32 | palette 10 × 4 × u32
//   nParts u8, pad u8, pad u16
//   per part: layer u8, paint u8, nSub u8, mask u8, stroke f32, nKeys u16,
//             flags u8 (bit0 lid-clipped, bit1 gaze limited to the unit disc), pad u8,
//             nSub × { closed u8, eye u8 (0, 1 left, 2 right), nPoints u16 }, align 4,
//             [if nKeys > 1] state map 416 × u16, align 4,
//             nKeys × key: opacity u8 + pad 3, P × (x i16, y i16),
//                          per delta set in the mask: scale f32, 2P × i8, align 4
static constexpr uint16_t BLOB_VERSION = 2;
#ifdef ESP_PLATFORM
static constexpr uint32_t PAGE = 0x10000;  // flash MMU page
#endif

static const char *error = "";

float rdF32(const uint8_t *p) {
  uint32_t v = rdU32(p);
  float f;
  memcpy(&f, &v, sizeof f);
  return f;
}

static inline uint32_t align4(uint32_t n) {
  return (n + 3) & ~3u;
}

// ─── Where the bytes are ────────────────────────────────────────────────────

static const uint8_t *memPack = nullptr;  // the whole pack in memory (host, tests)
static size_t memSize = 0;
static uint32_t packSize = 0;  // bytes the partition (or memory) holds
static bool packOk = false;

#ifdef ESP_PLATFORM
static const esp_partition_t *partition = nullptr;
static esp_partition_mmap_handle_t headHandle = 0, blobHandle = 0;
static const uint8_t *headMap = nullptr, *blobMap = nullptr;
static uint32_t headLen = 0, blobAt = 0, blobLen = 0;
#endif

// `size` bytes of the pack from `off`, or null when they are not there.
static const uint8_t *window(uint32_t off, uint32_t size) {
  if (memPack) return (uint64_t)off + size <= memSize ? memPack + off : nullptr;
#ifdef ESP_PLATFORM
  if (!partition || (uint64_t)off + size > packSize) return nullptr;
  if (headMap && off + size <= headLen) return headMap + off;
  if (blobMap && off >= blobAt && (uint64_t)off + size <= (uint64_t)blobAt + blobLen) return blobMap + (off - blobAt);
  // A new region: unmap the previous blob (the caller dropped its pointers) and map the pages that hold this one.
  if (blobMap) esp_partition_munmap(blobHandle);
  blobMap = nullptr;
  uint32_t from = off & ~(PAGE - 1);
  uint32_t len = (uint32_t)(((uint64_t)off + size - from + PAGE - 1) & ~(uint64_t)(PAGE - 1));
  if (from + len > partition->size) len = partition->size - from;
  const void *p = nullptr;
  if (esp_partition_mmap(partition, from, len, ESP_PARTITION_MMAP_DATA, &p, &blobHandle) != ESP_OK) return nullptr;
  blobMap = (const uint8_t *)p;
  blobAt = from;
  blobLen = len;
  return blobMap + (off - from);
#else
  return nullptr;
#endif
}

// Reads the pack header; false if it is not one.
static bool readHeader() {
  const uint8_t *h = window(0, 8);
  if (!h || memcmp(h, "MCP1", 4) != 0 || rdU16(h + 4) != 1) return error = "pack: magic", false;
  uint32_t count = rdU16(h + 6);
  if (!count || !window(0, 8 + count * 20)) return error = "pack: directory", false;
  return true;
}

void companionUsePack(const uint8_t *data, size_t size) {
  memPack = data;
  memSize = size;
  packSize = (uint32_t)size;
  packOk = data && readHeader();
}

bool companionOpenPack() {
  if (packOk) return true;
#ifdef ESP_PLATFORM
  if (!memPack) {
    // Data partition, subtype 0x40, labelled "cmp" (partitions.csv).
    if (!partition) partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "cmp");
    if (!partition) return error = "pack: no partition", false;
    packSize = partition->size;
    // The directory: the first page (mapped once, even when it turns out to hold no pack).
    if (!headMap) {
      headLen = packSize < PAGE ? packSize : PAGE;
      const void *p = nullptr;
      if (esp_partition_mmap(partition, 0, headLen, ESP_PARTITION_MMAP_DATA, &p, &headHandle) != ESP_OK) return error = "pack: map", false;
      headMap = (const uint8_t *)p;
    }
  }
#endif
  packOk = readHeader();
  return packOk;
}

// ─── Directory ──────────────────────────────────────────────────────────────

int companionCount() {
  if (!companionOpenPack()) return 0;
  return rdU16(window(0, 8) + 6);
}

const char *companionIdAt(int n) {
  static char id[13];
  if (n < 0 || n >= companionCount()) return "";
  const uint8_t *e = window(8 + n * 20, 20);
  if (!e) return "";
  memcpy(id, e, 12);
  id[12] = 0;
  return id;
}

// ─── Blob ───────────────────────────────────────────────────────────────────

static Companion current;
static bool haveCurrent = false;

const Companion *companionCurrent() {
  return haveCurrent ? &current : nullptr;
}

const char *companionError() {
  return error;
}

void companionPartKey(const CompPart &p, int state, PartKey &out) {
  int k = p.map ? rdU16(p.map + state * 2) : 0;
  const uint8_t *key = p.keys + (size_t)k * p.keySize;
  out.opacity = key[0];
  out.pts = key + 4;
  const uint8_t *at = key + 4 + 4 * (size_t)p.nPoints;
  for (int s = 0; s < SETS; s++) {
    if (p.mask & (1 << s)) {
      out.scale[s] = rdF32(at);
      out.delta[s] = at + 4;
      at += 4 + align4(2 * (uint32_t)p.nPoints);
    } else {
      out.scale[s] = 0;
      out.delta[s] = nullptr;
    }
  }
}

static bool fail(const char *why) {
  error = why;
  haveCurrent = false;
  return false;
}

// Parses the blob of `size` bytes at `b` into `c`; the keys stay where they are.
static bool parseBlob(const uint8_t *b, uint32_t size, Companion &c) {
  if (size < 512 || ((uintptr_t)b & 3)) return fail("blob: size or alignment");
  if (memcmp(b, "MCMP", 4) != 0) return fail("blob: magic");
  if (rdU16(b + 4) != BLOB_VERSION) return fail("blob: version");
  uint16_t flags = rdU16(b + 6);
  c.ownEyes = flags & 1;
  c.nocturnal = flags & 2;
  c.voice = (flags >> 2) & 7;
  uint32_t p = 8;
  float *e = &c.eyes.dx;  // EyeBuild is seven floats in the pack's order
  for (int i = 0; i < 7; i++, p += 4) e[i] = rdF32(b + p);
  for (int x = 0; x < E_COUNT; x++) {
    float *ex = &c.expr[x].lid;  // lid, tilt, low, happy, eyeScale, pupil
    for (int i = 0; i < 6; i++, p += 4) ex[i] = rdF32(b + p);
  }
  // The eyes' numbers feed the renderer's integer maths: a damaged table is refused, not drawn.
  auto within = [](float v, float lo, float hi) { return v >= lo && v <= hi; };  // false for NaN
  const EyeBuild &k = c.eyes;
  if (!(within(k.dx, 0, 120) && within(k.cy, 0, 180) && within(k.rx, 4, 120) && within(k.ry, 4, 120) && within(k.leftScale, 0.2f, 3) && within(k.pupilRatio, 0, 2) &&
        within(k.pupilAspect, 0.05f, 3)))
    return fail("blob: eye build");
  for (int x = 0; x < E_COUNT; x++) {
    const Expression &ex = c.expr[x];
    if (!(within(ex.lid, -1, 2) && within(ex.tilt, -3, 3) && within(ex.low, -1, 2) && within(ex.happy, -1, 2) && within(ex.eyeScale, 0.2f, 3) && within(ex.pupil, 0, 3)))
      return fail("blob: expression");
  }
  for (int r = 0; r < PAL_ROWS; r++)
    for (int i = 0; i < 4; i++, p += 4) c.palette[r][i] = rdU32(b + p) & 0xFFFFFF;
  c.nParts = b[p];
  p += 4;
  if (c.nParts > COMP_MAX_PARTS) return fail("blob: too many parts");

  c.nSubs = c.nPoints = c.nStrokePoints = 0;
  for (int i = 0; i < c.nParts; i++) {
    CompPart &q = c.parts[i];
    if (p + 12 > size) return fail("blob: truncated");
    q.layer = b[p];
    q.paint = b[p + 1];
    q.nSub = b[p + 2];
    q.mask = b[p + 3];
    q.stroke = rdF32(b + p + 4);
    q.nKeys = rdU16(b + p + 8);
    q.clip = b[p + 10] & 1;
    q.gazeDisc = (b[p + 10] >> 1) & 1;
    p += 12;
    if (q.layer > 1 || q.paint > PAINT_BG || (q.mask & ~0xF) || !q.nKeys || (q.clip && q.stroke > 0)) return fail("blob: part header");
    if (c.nSubs + q.nSub > COMP_MAX_SUBS || p + 4u * q.nSub > size) return fail("blob: too many sub-paths");
    q.firstSub = c.nSubs;
    q.nPoints = 0;
    int closed = 0;
    for (int j = 0; j < q.nSub; j++, p += 4) {
      CompSub &s = c.subs[c.nSubs++];
      s.closed = b[p] != 0;
      s.eye = b[p + 1];
      if (s.eye > 2) return fail("blob: sub-path eye");
      s.n = rdU16(b + p + 2);
      q.nPoints += s.n;
      closed += s.closed;
    }
    p = align4(p);
    q.nSets = 0;
    for (int s = 0; s < SETS; s++) q.nSets += (q.mask >> s) & 1;
    q.map = nullptr;
    if (q.nKeys > 1) {
      if (p + COMP_STATES * 2 > size) return fail("blob: truncated map");
      q.map = b + p;
      for (int s = 0; s < COMP_STATES; s++)
        if (rdU16(q.map + s * 2) >= q.nKeys) return fail("blob: map entry");
      p = align4(p + COMP_STATES * 2);
    }
    q.keySize = align4(4 + 4 * (uint32_t)q.nPoints + q.nSets * (4 + align4(2 * (uint32_t)q.nPoints)));
    q.keys = b + p;
    if ((uint64_t)p + (uint64_t)q.keySize * q.nKeys > size) return fail("blob: truncated keys");
    p += q.keySize * q.nKeys;
    c.nPoints += q.nPoints + closed;
    if (q.stroke > 0) c.nStrokePoints += q.nPoints + closed;
  }
  if (c.nPoints > COMP_MAX_POINTS || c.nStrokePoints > COMP_MAX_STROKE_POINTS) return fail("blob: too many points");
  return true;
}

const Companion *companionSelect(const char *id) {
  haveCurrent = false;
  error = "";
  if (!id || !*id || !strcmp(id, "madre")) return nullptr;
  if (strlen(id) > 12) {
    error = "unknown id";
    return nullptr;
  }
  if (!companionOpenPack()) return nullptr;
  const uint8_t *dir = window(0, 8);
  uint32_t count = rdU16(dir + 6);
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *e = window(8 + i * 20, 20);
    if (!e || strncmp((const char *)e, id, 12) != 0) continue;
    uint32_t off = rdU32(e + 12), size = rdU32(e + 16);
    const uint8_t *b = window(off, size);
    if (!b) {
      error = "blob: outside the pack";
      return nullptr;
    }
    strncpy(current.id, id, 12);
    current.id[12] = 0;
    haveCurrent = parseBlob(b, size, current);
    return haveCurrent ? &current : nullptr;
  }
  error = "unknown id";
  return nullptr;
}
