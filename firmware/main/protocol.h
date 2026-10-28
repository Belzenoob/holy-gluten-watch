#pragma once
#include <stdint.h>

// BLE contract with the app. Mirrors src/domain/watch.ts: change both together.

#define MADRE_DEVICE_NAME "MADRE"
#define MADRE_FW_VERSION "0.3.0"
#define MADRE_PROTOCOL 2

#define MADRE_SERVICE_UUID "8f1c0001-5b7e-4b8a-9c3d-6d6164726501"
// Write (with response): the starter's state, split in frames. JSON:
//   {"v":2,"now":<unix s>,"tz":<minutes east of UTC>,"name":"Madre","r":[<unix s>, …]}
// "r" is the refresh history, newest first, at most MADRE_MAX_REFRESHES.
// Optional (firmware 0.3.0 on; older firmware ignores it, so the protocol stays 2):
//   "c":"<id>"  the companion to draw (companion.h); absent = Madre.
//     Kept in NVS (madre/comp); an unknown id or a missing pack shows Madre.
// Optional, for the app's test panel:
//   "preview":{"mood":"active","night":false}  shows that state instead of the
//     real one (the screen stays on) until a state without it arrives;
//   "act":"giggle"|"annoy"|"grumpy"|"feed"|"quirk"  plays that reaction once.
#define MADRE_STATE_UUID "8f1c0002-5b7e-4b8a-9c3d-6d6164726501"
// Read + notify: JSON status
//   {"p":2,"fw":"0.3.0","fed":[<unix s>, …],"bat":<0..100 or -1>,"ok":true,"err":"…"}
// "fed" lists refreshes logged on the watch that the last state did not
// contain yet. The phone adds them and writes the state back; the watch then
// drops every pending refresh that the new state contains.
#define MADRE_STATUS_UUID "8f1c0003-5b7e-4b8a-9c3d-6d6164726501"

// First byte of every frame written to STATE.
//   BEGIN: u32 LE total payload length.
//   DATA:  payload bytes, in order.
//   END:   u32 LE CRC-32 (IEEE, as zlib) of the whole payload.
enum : uint8_t { FRAME_BEGIN = 1, FRAME_DATA = 2, FRAME_END = 3 };

#define MADRE_MAX_PAYLOAD 4096
#define MADRE_MAX_REFRESHES 16
// A watch refresh and a phone refresh this close are the same one.
#define MADRE_SAME_REFRESH_S 120
