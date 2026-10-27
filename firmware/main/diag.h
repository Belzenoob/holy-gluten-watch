#pragma once
#include <stdint.h>

// Power diagnosis with nothing but the board itself. The AXP2101 measures
// the battery voltage but not the current, so the current comes from two
// effects of the battery's internal resistance:
//   - step: switching one consumer off and on makes the voltage jump by
//     current × resistance; many fast cycles average the noise away.
//   - calibration: with USB in, switching the charger off and on is a step of
//     a known current (the charge current), which gives the resistance.
// On battery a scheduled run also compares the voltage slope over blocks of
// a few tens of minutes: normal, BLE off, normal again.
// "diagnosi suolo": the absolute standby current. Three blocks, asleep /
// awake at 40 MHz / asleep, each timing the voltage slope; the awake block
// adds the current the "sonno" step measured (a known extra load), which
// turns volts per hour into milliamps per volt per hour, and so the slope of
// the sleeping blocks into the sleeping current.
// Results survive restarts (NVS); "diagnosi" prints them.

// A consumer the diagnosis switches; on = as in normal sleep.
struct Knob {
  const char *name;
  void (*set)(bool on);
  uint16_t settleMs = 0;  // after each switch; 0 = the default 600 ms
};

enum DiagMode { DIAG_IDLE, DIAG_NORMAL };

// stopBle: BLE down for good (until restart).
void diagBegin(const Knob *knobs, int count, void (*stopBle)());
// Knobs measured with the chip asleep between readings (what only matters in
// light sleep, such as how the pins are left), for "diagnosi passi".
void diagSleepKnobs(const Knob *knobs, int count);
// Called every loop while the screen is off; blocking while a step series runs.
DiagMode diagTick(uint32_t ms, bool usb);
// Something woke the screen: a floor run in progress starts its block again
// (the measurement is only worth anything with the watch left alone).
void diagDisturbed();

// "diagnosi profondo": the same slope with the chip in deep sleep (its own
// current gone, the pins held as in light sleep). After a settle and a light
// sleep block of reference, `prepare` holds the pins and the chip sleeps deep
// for a while; the timer wakes it (a restart), and diagEarlyBoot(), right
// after hwBegin(), reads the voltage before anything else powers up.
void diagDeepHook(void (*prepare)());
void diagEarlyBoot();
bool diagCommand(const char *cmd);  // "diagnosi ...", "calibra", "passo <knob>", "manopola <knob> 0|1", "vbat"
