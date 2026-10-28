#pragma once
#include <stdint.h>

// Companion voices: the five sounds a companion makes in a voice of its own (hello, giggle, hungry,
// annoyed, munch), read from the `cmp` data partition at VOICES_OFFSET and streamed to the codec in
// small chunks, so they cost no RAM. Voice 0 is the built-in set (sounds.cpp: Madre and the common
// companions); a voice that lacks a sound falls back to the built-in one.
//   "MSND" | version u16 = 1 | nClips u16
//   Clip[nClips]: voice u8 | sound u8 | pad u16 | offset u32 (from VOICES_OFFSET) | count u32 (samples)
//   16 kHz mono int16 LE samples, each clip 4-aligned.
// Without that table every companion plays the built-in set.

static constexpr uint32_t VOICES_OFFSET = 0x600000;

// Reads the clip table; false when the partition or the table is missing (every voice is then the built-in one).
bool voicesOpen();
// Where a voice's clip lives (flash offset from VOICES_OFFSET, samples), or false when it has none.
bool voiceClip(uint8_t voice, uint8_t sound, uint32_t &offset, uint32_t &count);
// Copies `n` samples starting at sample `from` of the clip at `offset`.
bool voiceRead(uint32_t offset, uint32_t from, int16_t *dst, uint32_t n);
