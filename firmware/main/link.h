#pragma once
#include <Arduino.h>
#include <string>

// BLE link to the phone (protocol.h). Callbacks run on the BLE task; the
// main loop polls.

// The BLE stack, on only while the watch is awake (and a little after): even
// idle, it keeps the chip out of its deep light sleep (~9 mA of ~12).
void linkStart();
void linkStop();
bool linkRunning();
bool linkConnected();
// True once a complete STATE with a matching CRC has arrived; moves it into `out`.
bool linkTakePayload(std::string &out);
// Called on the BLE task when a payload is ready (to wake the sleeping loop).
void linkOnPayload(void (*cb)());
// Next link event for the log (connections, transfers, errors), if any.
bool linkTakeEvent(char *out, size_t size);
// Publishes STATUS: refreshes logged here and not yet confirmed, battery, last result.
void linkReportStatus(const int64_t *fed, size_t n, int battery, bool ok, const char *err);
// Diagnostics: adverts on or off.
void linkAdvertise(bool on);
