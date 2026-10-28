#include "voices.h"
#include <string.h>
#include "esp_partition.h"

static constexpr int MAX_CLIPS = 64;
struct VoiceClip {
  uint8_t voice, sound;
  uint32_t offset, count;
};
static VoiceClip clips[MAX_CLIPS];
static int nClips = 0;
static const esp_partition_t *partition = nullptr;

static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool voicesOpen() {
  nClips = 0;
  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "cmp");
  if (!partition || partition->size < VOICES_OFFSET + 8) return false;
  uint8_t head[8];
  if (esp_partition_read(partition, VOICES_OFFSET, head, sizeof(head)) != ESP_OK || memcmp(head, "MSND", 4) != 0) return false;
  int n = head[6] | (head[7] << 8);
  if (n > MAX_CLIPS) n = MAX_CLIPS;
  for (int i = 0; i < n; i++) {
    uint8_t row[12];
    if (esp_partition_read(partition, VOICES_OFFSET + 8 + i * 12, row, sizeof(row)) != ESP_OK) break;
    uint32_t offset = u32(row + 4), count = u32(row + 8);
    if (offset + count * 2 > partition->size - VOICES_OFFSET) continue;  // damaged row: skip it
    clips[nClips++] = {row[0], row[1], offset, count};
  }
  return nClips > 0;
}

bool voiceClip(uint8_t voice, uint8_t sound, uint32_t &offset, uint32_t &count) {
  for (int i = 0; i < nClips; i++)
    if (clips[i].voice == voice && clips[i].sound == sound) {
      offset = clips[i].offset;
      count = clips[i].count;
      return true;
    }
  return false;
}

bool voiceRead(uint32_t offset, uint32_t from, int16_t *dst, uint32_t n) {
  return partition && esp_partition_read(partition, VOICES_OFFSET + offset + from * 2, dst, n * 2) == ESP_OK;
}
