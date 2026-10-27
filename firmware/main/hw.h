#pragma once
#include <stdint.h>
#include <stddef.h>

// Board peripherals on the shared I2C bus: real-time clock (PCF85063, kept
// in UTC), accelerometer (QMI8658) and power manager (AXP2101).

void hwBegin();

// Unix seconds, 0 while the clock has never been set by the phone.
int64_t clockNow();
void clockSet(int64_t unix);

// Latest acceleration in g, low-latency; false if the sensor is missing.
bool motionRead(float &x, float &y, float &z);

// Full rate while the screen is on; low-power sampling while it is off.
// Ignored while the tap detector runs (motionTapWake).
void motionFast(bool fast);

// Tap to wake: the accelerometer's own tap detector raises INT2 (GPIO17)
// while the chip sleeps. The sensor then runs at the detector's rate all the
// time: 250 Hz by default ("motionTapRate" for 128 low-power, where the
// detector stayed silent, or 500 Hz, the datasheet's example rate).
void motionTapWake(bool on);
bool motionTapWakeOn();
void motionTapRate(int hz);
int motionTapRateHz();
// Reads and clears the sensor's event flags: 0 none, 1 single tap, 2 double tap.
// `status1` gets the STATUS1 register as read (bit 1 = a new tap).
int motionTapEvent(int *status1 = nullptr);
// Which sensor pin carries the tap events: 1 (INT1, GPIO16) or 2 (INT2, GPIO17).
void motionTapPin(int pin);
int motionTapPinNo();
// The tap detector's thresholds, in g: a peak above `peak` counts, below
// `quiet` is rest (the datasheet example says 0.8 / 0.4: too much for a
// finger on a watch).
void motionTapThreshold(float peak, float quiet);
// Data-ready pulses on INT2 (an experiment: CTRL7 bit 5).
void motionDrdy(bool on);
// One line with the sensor's control and status registers.
void motionRegs(char *out, size_t size);

// Battery percentage, -1 when unknown or on USB without a battery.
int batteryPercent();
// The AMOLED panel's own power supply (AXP2101 ALDO2 drives the panel's
// DSI_PWR_EN): off while the screen sleeps, so the panel really draws nothing.
void panelPower(bool on);

// Which power-manager rails are on, and at what voltage (diagnostics).
void railsReport(char *out, size_t size);

// Battery voltage in millivolts, 0 when unknown.
int batteryMillivolts();

// USB power present (the power manager sees VBUS). Unlike the USB console's
// own detection it does not miss frames while the chip sleeps.
bool usbPowered();

// Diagnostics: raw AXP2101 registers, the charger, and the rails and sensors
// the power measurement switches one at a time.
int pmuRead(uint8_t reg);
bool pmuWrite(uint8_t reg, uint8_t value);
void chargerEnable(bool on);
int chargerCurrentMa();
const char *chargerState();
// 0 on battery, 1 pre-charge, 2 constant current, 3 constant voltage, 4 full, 5 other.
int chargerPhase();
// ALDO1: codec and microphone ADC. Keep it on: off, the two chips are
// back-powered from the I2C pull-ups (+2.4 mA) and can lock the bus.
void audioRail(bool on);
void motionPower(bool on);  // accelerometer on (low-power rate) or off
