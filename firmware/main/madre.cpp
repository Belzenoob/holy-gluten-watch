// MADRE on the wrist: the sourdough starter's tamagotchi for the Waveshare
// ESP32-C6-Touch-AMOLED-2.06. The same pet as the app's Lievito tab (brain,
// expressions, behaviour), kept in sync with the phone over BLE. Minimal on
// purpose: only the eyes; a long press shows satiety and vitality.
// Display and touch setup follow Waveshare's 06_LVGL_Arduino_v9 example.
//
// ESP-IDF build (firmware/madre-c6-idf): same behaviour as the Arduino one,
// plus power management. With the screen off the chip sleeps on its own
// between checks (automatic light sleep) and so does the BLE radio between
// adverts; while the screen is on, two locks keep it awake and at full speed.

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "HWCDC.h"
#include "protocol.h"
#include "brain.h"
#include "pet.h"
#include "stage.h"
#include "store.h"
#include "hw.h"
#include "link.h"
#include "ui.h"
#include "audio.h"
#include "diag.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include <Preferences.h>

HWCDC USBSerial;

static constexpr uint32_t LVGL_BUFFER_LINES = 20;
static constexpr int STRIP_ROWS = 20;  // stage rows per push (even: the CO5300 wants even rows)
static constexpr uint32_t FRAME_MS = 33;
static constexpr uint8_t BRIGHTNESS_ON = 0xC0;
// Power: the screen stays on 10 s after the last interaction, fades, and the
// panel goes to sleep. Asleep, the side key or a double tap on the glass (the
// accelerometer's own detector, see "Waking from a dark screen") wakes it,
// or the phone: no raise-to-wake or shake-to-wake, and nothing polls while
// the chip is in light sleep.
static constexpr uint32_t AWAKE_MS = 10 * 1000;
static constexpr uint32_t FADE_MS = 600;
// Asleep, the loop waits this long for a wake pin or a payload to wake it
// (task notification): nothing to poll in between, every wake costs a few
// milliseconds at 40 MHz. Under the 8 s watchdog: with the chip held awake
// (the power diagnosis does it) a longer wait is a crash (seen, 30 s). With
// USB in, shorter, so the console stays snappy.
static constexpr uint32_t ASLEEP_WAIT_MS = 5000;
static constexpr uint32_t ASLEEP_WAIT_USB_MS = 120;
static constexpr uint32_t ASLEEP_THINK_MS = 30 * 1000;
// Bluetooth is on while the screen is, and this long after the last touch:
// the phone (which retries every 2 s while the app is open) syncs right
// after you look at the watch, and not otherwise: even idle, the stack keeps
// the chip from sleeping deeply (~9 mA of ~12). A sync under way may run on
// a while longer. With USB in it stays on (no battery to spare, the app can test).
static constexpr uint32_t BLE_WINDOW_MS = 20 * 1000;
static constexpr uint32_t BLE_SYNC_GRACE_MS = 60 * 1000;
// The side key (PWR): pressed = GPIO18 high, through T1 on the board ("KEYS"
// on the schematic; the power manager's own IRQ pin goes nowhere).
static constexpr int PIN_PWR_KEY = 18;
// The accelerometer's INT1 and INT2: GPIO16 and GPIO17, also U0TXD/U0RXD, so
// the IDF console is off (sdkconfig) and the pins are ours. The tap events
// go to one of them (motionTapPin); the ROM drives U0TXD (INT1's pin) at
// every boot, so INT2 is preferred when it works.
static constexpr int PIN_IMU_INT1 = 16;
static constexpr int PIN_IMU_INT2 = 17;
static int PIN_IMU_INT = PIN_IMU_INT2;
static volatile uint32_t imuEdges[2] = {0, 0};  // rising edges seen on GPIO16 / GPIO17 (diagnostics)
static constexpr uint32_t STATS_HOLD_MS = 2000;   // long press to show the stats
static constexpr uint32_t STATS_LINGER_MS = 3000;  // they stay this long after release
// A refresh from the phone this recent is celebrated on the watch.
static constexpr int64_t FRESH_REFRESH_S = 15 * 60;

// "Shared" makes the library take the SPI bus only while it draws: otherwise
// it holds it (and with it a power lock) forever and the chip never sleeps.
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, true);
// The panel forgets its setup when its power (ALDO2) is cut while asleep:
// reinit() runs the driver's power-on sequence again (reset, setup, on).
class Panel : public Arduino_CO5300 {
 public:
  using Arduino_CO5300::Arduino_CO5300;
  void reinit() {
    tftInit();
    setRotation(0);
  }
};
Panel *gfx = new Panel(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void onTouchInterrupt(void);
std::unique_ptr<Arduino_IIC> touch(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, onTouchInterrupt));

static uint32_t screenWidth, screenHeight;
static bool touchReady = false;

static Saved saved;
static Pet pet;
static StageFrame frame;
static uint16_t strip[STAGE_W * STRIP_ROWS];

enum Screen { SCREEN_ON, SCREEN_FADING, SCREEN_OFF };
static Screen screen = SCREEN_ON;
static uint32_t lastActivity = 0, fadeStart = 0;

// Touch: every touch belongs to the pet.
static bool touchDown = false, swallowTouch = false;
// Reads that failed in a row; past a threshold the I2C bus is restarted
// (a bus stuck after a sleep would otherwise leave the watch deaf to touch).
static int touchFails = 0, i2cRecoveries = 0;
static int touchX = 0, touchY = 0, touchStartX = 0, touchStartY = 0;
static uint32_t touchSince = 0, statsUntil = 0;
static bool statsOn = false;

static int battery = -1;
static PetState brain;


// Power locks: "awake" keeps the CPU out of light sleep and "fast" at full
// speed while the screen is on; "usb" keeps it awake, at 80 MHz, while
// plugged in, so the USB console keeps working.
static esp_pm_lock_handle_t awakeLock, fastLock, usbLock;
static bool usbHeld = false;

static void powerBegin() {
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = 160;
  pm.min_freq_mhz = 40;
  pm.light_sleep_enable = true;
  esp_pm_configure(&pm);
  esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "screen", &awakeLock);
  esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "render", &fastLock);
  // APB at 80 MHz, not just no light sleep: at 40 MHz the chip runs from the
  // crystal, the PLL stops and the USB PHY with it (the port went dead with
  // the screen off). A frequency lock also keeps it out of light sleep.
  esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb", &usbLock);
  esp_pm_lock_acquire(awakeLock);  // the screen starts on
  esp_pm_lock_acquire(fastLock);
}

// Battery log, to measure real consumption: every 5 minutes the charge, the
// voltage and how long the screen was on; "batteria" over USB prints it.
struct BatterySample {
  uint16_t minute;      // since boot
  int8_t percent;
  uint16_t millivolts;
  uint16_t screenOnS;   // in the 5 minutes before
  uint8_t usb;          // plugged in (charging, never asleep)
  uint8_t charger;      // chargerPhase(): constant current, constant voltage, full
};
static constexpr int LOG_SIZE = 288;  // 24 hours
static BatterySample batteryLog[LOG_SIZE];
static int logCount = 0;
static uint32_t screenOnMs = 0, lastLogAt = 0;

static void logBattery(uint32_t ms) {
  BatterySample s = {(uint16_t)(ms / 60000), (int8_t)batteryPercent(), (uint16_t)batteryMillivolts(), (uint16_t)(screenOnMs / 1000), (uint8_t)usbHeld, (uint8_t)chargerPhase()};
  if (logCount == LOG_SIZE) {
    memmove(batteryLog, batteryLog + 1, sizeof(BatterySample) * (LOG_SIZE - 1));
    logCount--;
  }
  batteryLog[logCount++] = s;
  screenOnMs = 0;
}

// Charger phase changes, to the second (the 5-minute log is too coarse to
// time the constant-current phase, which gives the battery's capacity).
struct PhaseChange {
  uint32_t second;
  uint8_t phase;
  int8_t percent;
  uint16_t millivolts;
};
static PhaseChange phaseChanges[12];
static int nPhaseChanges = 0, lastPhase = -1;

static void watchCharger(uint32_t ms) {
  static uint32_t lastCheck = 0;
  if (ms - lastCheck < 5000) return;
  lastCheck = ms;
  int phase = chargerPhase();
  if (phase == lastPhase) return;
  lastPhase = phase;
  if (nPhaseChanges == 12) {
    memmove(phaseChanges, phaseChanges + 1, sizeof(PhaseChange) * 11);
    nPhaseChanges--;
  }
  phaseChanges[nPhaseChanges++] = {ms / 1000, (uint8_t)phase, (int8_t)batteryPercent(), (uint16_t)batteryMillivolts()};
}

static void printBatteryLog() {
  static const char *const PHASES[] = {"-", "precarica", "corrente costante", "tensione costante", "completa", "?"};
  USBSerial.println("minuto  %   mV  schermo_s  usb  carica");
  for (int i = 0; i < logCount; i++)
    USBSerial.printf("%6u %3d %5u %9u %4u  %s\n", batteryLog[i].minute, batteryLog[i].percent, batteryLog[i].millivolts, batteryLog[i].screenOnS, batteryLog[i].usb,
                     PHASES[batteryLog[i].charger > 5 ? 5 : batteryLog[i].charger]);
  // Capacity from the charge: the set current times the time at constant
  // current (plus whatever went in at constant voltage, not counted here).
  USBSerial.println("cambi di fase della carica (secondo, fase, %, mV):");
  for (int i = 0; i < nPhaseChanges; i++) {
    const PhaseChange &c = phaseChanges[i];
    USBSerial.printf("  %6u s  %-18s %3d%% %5u mV", c.second, PHASES[c.phase > 5 ? 5 : c.phase], c.percent, c.millivolts);
    if (c.phase == 3 && i > 0 && phaseChanges[i - 1].phase == 2) {
      uint32_t s = c.second - phaseChanges[i - 1].second;
      USBSerial.printf("  <- %u min a corrente costante = %u mAh", s / 60, s * chargerCurrentMa() / 3600);
    }
    USBSerial.println();
  }
  // Average drop on battery only (the longest stretch without USB).
  int from = -1, to = -1;
  for (int i = 0; i < logCount; i++) {
    if (batteryLog[i].usb) {
      from = -1;
      continue;
    }
    if (from < 0) from = i;
    to = i;
  }
  if (from >= 0 && to > from) {
    float hours = (batteryLog[to].minute - batteryLog[from].minute) / 60.0f;
    int drop = batteryLog[from].percent - batteryLog[to].percent;
    int mv = batteryLog[from].millivolts - batteryLog[to].millivolts;
    USBSerial.printf("a batteria: %.1f ore, -%d%%, -%d mV", hours, drop, mv);
    if (drop > 0) USBSerial.printf(", circa %.1f%% all'ora: durata stimata %.0f ore", drop / hours, 100 / (drop / hours));
    USBSerial.println();
  }
}

// From the power manager, not USBSerial.isPlugged(): that one watches USB
// frames from the timer tick, which tickless sleep skips; it then reported
// "unplugged" with the cable in, the chip slept and the USB port went dead
// (esptool too) until the screen was touched.
static void powerUsb() {
  bool plugged = usbPowered();
  if (plugged == usbHeld) return;
  usbHeld = plugged;
  if (plugged) esp_pm_lock_acquire(usbLock);
  else esp_pm_lock_release(usbLock);
}

// The app's test panel: a forced state, shown until the panel closes.
static constexpr uint32_t PREVIEW_MAX_MS = 15 * 60 * 1000;  // never stuck in a test
static bool previewOn = false;
static Mood previewMood = NEWBORN;
static bool previewNight = false;
static uint32_t previewSince = 0;

// Diagnostics ("log" over USB): input events with timestamps, and the slowest
// pass of each part of the loop since the last "stato".
static bool eventLog = false;
enum Part { P_TOUCH, P_MOTION, P_LVGL, P_STAGE, P_LOOP, PARTS };
static const char *const PART_NAMES[PARTS] = {"touch", "accelerometro", "lvgl", "occhi", "ciclo"};
static uint32_t partMax[PARTS];
static inline void timed(Part p, uint32_t since) {
  uint32_t d = micros() - since;
  if (d > partMax[p]) partMax[p] = d;
}

// The touch controller pulls TP_INT low when touched, also from its
// low-power monitor mode, where it does not answer on I2C at all: the pin is
// the only way to know. Screen on: falling edge. Screen off: a low level,
// which also wakes the chip from light sleep (an edge is not seen asleep);
// the first one disables the interrupt so a held finger cannot flood it.
static volatile bool touchIrq = false;
static volatile bool touchLevel = false;  // the interrupt is level-triggered (screen off)
static TaskHandle_t loopTask = nullptr;   // waits for a touch while the screen is off

void onTouchInterrupt(void) {
  touchIrq = true;
  // Its own flag, not `screen`: wake() switches back to edges before the
  // screen counts as on, and a finger still down fires in between; checking
  // `screen` there switched the interrupt off for good (taps stopped working
  // after a touch had woken the watch).
  if (touchLevel) gpio_intr_disable((gpio_num_t)TP_INT);
  if (loopTask) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(loopTask, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

static void touchWakeSource(bool asleep) {
  gpio_num_t pin = (gpio_num_t)TP_INT;
  if (asleep) {
    touchLevel = true;
    gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);  // makes the interrupt level-triggered too
  } else {
    gpio_intr_disable(pin);
    touchLevel = false;
    gpio_wakeup_disable(pin);
    gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
  }
  gpio_intr_enable(pin);
}

static uint32_t millisCb(void) {
  return millis();
}

static void think();

static bool bleBlocked = false;  // the power diagnosis turned it off for a block

// ─── Waking from a dark screen ──────────────────────────────────────────────
// How the watch wakes while the screen is off (NVS "wake", command "sveglia"):
//   WAKE_KEY    the side key only. The touch controller hibernates: in its
//               monitor mode it costs 1.7 mA, more than all the rest asleep.
//   WAKE_TAP    the key, or any firm knock on the glass: the accelerometer's
//               own tap detector raises INT2 while the chip sleeps (default;
//               a knock now and then from a bump costs 10 s of screen).
//   WAKE_DOUBLE the key, or a double knock only: fewer stray wakes, but the
//               detector reads a double as a single now and then (seen).
//   WAKE_TOUCH  the old way: the touch controller keeps scanning, one tap wakes.
enum WakeMode : uint8_t { WAKE_KEY, WAKE_TAP, WAKE_TOUCH, WAKE_DOUBLE };
// Default: the touch keeps listening (one tap wakes, as always). Measured
// 2026-10-24: the board draws ~20 mA asleep whatever the chip does (deep sleep
// only 8 % less), so hibernating the touch saves 0.5 mA, 2 %: not worth the knock.
static WakeMode wakeMode = WAKE_TOUCH;
static bool tapWakes() {
  return wakeMode == WAKE_TAP || wakeMode == WAKE_DOUBLE;
}
static volatile bool keyIrq = false, imuIrq = false;
static volatile bool pinsLevel = false;  // key/IMU interrupts are level-triggered (screen off)
static bool touchHibernated = false;
static uint32_t touchReviveAt = 0;  // after a reset pulse, when to talk to the controller again
static uint32_t touchModeAt = 0;    // when to send the (best-effort) monitor order after a revival
static uint32_t imuRearmAt = 0;     // INT2 stuck high: when to listen to it again

static void wake();
static void touchReset();

static void notifyLoopFromIsr() {
  if (!loopTask) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(loopTask, &woken);
  portYIELD_FROM_ISR(woken);
}

void onKeyInterrupt() {
  keyIrq = true;
  if (pinsLevel) gpio_intr_disable((gpio_num_t)PIN_PWR_KEY);  // held down: no flood
  notifyLoopFromIsr();
}

static void imuInterrupt(int pin) {
  imuEdges[pin == PIN_IMU_INT1 ? 0 : 1]++;
  if (pin != PIN_IMU_INT) return;  // the other pin only counts
  imuIrq = true;
  if (pinsLevel) gpio_intr_disable((gpio_num_t)pin);
  notifyLoopFromIsr();
}

void onImuInterrupt1() {
  imuInterrupt(PIN_IMU_INT1);
}

void onImuInterrupt2() {
  imuInterrupt(PIN_IMU_INT2);
}

// Asleep: a high level on the pin wakes the chip from light sleep (an edge is
// not seen asleep); awake: rising edges.
static void levelWake(int pin, bool asleep) {
  gpio_num_t g = (gpio_num_t)pin;
  if (asleep) {
    gpio_wakeup_enable(g, GPIO_INTR_HIGH_LEVEL);
  } else {
    gpio_intr_disable(g);
    gpio_wakeup_disable(g);
    gpio_set_intr_type(g, GPIO_INTR_POSEDGE);
  }
  gpio_intr_enable(g);
}

static void wakeSources(bool asleep) {
  pinsLevel = asleep;
  levelWake(PIN_PWR_KEY, asleep);
  if (tapWakes()) levelWake(PIN_IMU_INT, asleep);
}

// The controller's power mode, with the pause it needs: right after begin()
// (whose own setup order already selects monitor mode) it refuses every
// write for a moment, and in monitor mode it answers nothing at all (seen:
// the orders were silently lost).
static bool touchSetMode(uint8_t mode, int attempts) {
  for (int i = 0; i < attempts; i++) {
    delay(100);
    if (touch->IIC_Write_Device_State(touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE, mode)) return true;
  }
  return false;
}

// The touch controller hibernates (tens of µA instead of 1.7 mA); it answers
// nothing until a reset pulse. A reset first, which boots it awake and
// listening, then the order.
static void touchHibernate() {
  touchReset();
  bool ok = touch->begin() && touchSetMode(touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_HIBERNATE, 4);
  gpio_intr_disable((gpio_num_t)TP_INT);  // begin() attached it again; nothing to hear
  gpio_wakeup_disable((gpio_num_t)TP_INT);
  touchIrq = false;
  if (eventLog) USBSerial.printf("%7u touch %s\n", millis(), ok ? "ibernato" : "NON ibernato");
  touchReady = false;
  touchDown = false;
  touchHibernated = true;
}

// Out of hibernation: the reset pulse now, so the controller boots while the
// screen comes on; the loop talks to it a moment later (touchReviveTick).
static void touchRevive() {
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  gpio_sleep_sel_dis((gpio_num_t)TP_RESET);
  touchReviveAt = millis() + 150;
}

static void touchReviveTick(uint32_t ms) {
  if (touchModeAt && (int32_t)(ms - touchModeAt) >= 0) {
    touchModeAt = 0;
    // Best effort: the controller drops to monitor mode by itself anyway.
    if (touchReady) touch->IIC_Write_Device_State(touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE, touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  }
  if (!touchHibernated || (int32_t)(ms - touchReviveAt) < 0) return;
  touchReady = touch->begin();
  if (touchReady) {
    touchFails = 0;
    touchHibernated = false;
    touchWakeSource(false);  // begin() attached the interrupt again: edges, enabled
    touchModeAt = ms + 100;
    if (eventLog) USBSerial.printf("%7u touch sveglio\n", ms);
  } else {
    touchReviveAt = ms + 200;  // keeps trying while the screen is on
  }
}

// The key or the accelerometer fired: asleep, that is the wake; awake, the
// key just keeps the screen on.
static void wakePinsTick(uint32_t ms) {
  if (keyIrq) {
    keyIrq = false;
    if (eventLog) USBSerial.printf("%7u tasto laterale%s\n", ms, screen == SCREEN_OFF ? " a schermo spento" : "");
    if (screen == SCREEN_OFF) wake();
    else lastActivity = ms;
  }
  if (imuIrq) {
    imuIrq = false;
    int status1 = 0;
    int tap = motionTapEvent(&status1);  // reads the sensor's flags, which drops the pin
    if (eventLog)
      USBSerial.printf("%7u accelerometro: %s (STATUS1 %02X)%s\n", ms, tap == 2 ? "doppio tocco" : tap == 1 ? "tocco singolo" : "evento", status1, screen == SCREEN_OFF ? " a schermo spento" : "");
    if (screen == SCREEN_OFF) {
      if (tap == 2 || (tap == 1 && wakeMode == WAKE_TAP)) wake();
      else if (digitalRead(PIN_IMU_INT)) imuRearmAt = ms + 1000;  // still high: listen again later
      else levelWake(PIN_IMU_INT, true);                           // the ISR switched itself off
    }
  }
  if (imuRearmAt && (int32_t)(ms - imuRearmAt) >= 0) {
    imuRearmAt = 0;
    if (screen == SCREEN_OFF && tapWakes()) levelWake(PIN_IMU_INT, true);
  }
}

static const char *wakeModeName(WakeMode m) {
  return m == WAKE_KEY ? "tasto" : m == WAKE_TAP ? "tocco" : m == WAKE_DOUBLE ? "doppio" : "schermo";
}

static void wakeBegin() {
  Preferences p;
  if (p.begin("madre", true)) {
    uint8_t m = p.getUChar("wake", WAKE_TOUCH);
    if (m <= WAKE_DOUBLE) wakeMode = (WakeMode)m;
    p.end();
  }
  pinMode(PIN_PWR_KEY, INPUT);  // T1 and R11 define both levels
  gpio_sleep_sel_dis((gpio_num_t)PIN_PWR_KEY);
  attachInterrupt(PIN_PWR_KEY, onKeyInterrupt, RISING);
  for (int pin : {PIN_IMU_INT1, PIN_IMU_INT2}) {
    pinMode(pin, INPUT_PULLDOWN);  // high-impedance from the sensor until its detector drives it
    gpio_sleep_sel_dis((gpio_num_t)pin);
  }
  attachInterrupt(PIN_IMU_INT1, onImuInterrupt1, RISING);
  attachInterrupt(PIN_IMU_INT2, onImuInterrupt2, RISING);
  motionTapWake(tapWakes());
}

// "sveglia" over USB: shows or sets how the watch wakes (saved), or the tap
// detector's rate for a test ("sveglia odr 250").
static void wakeCommand(const char *arg) {
  if (!strncmp(arg, "odr ", 4)) {
    motionTapRate(atoi(arg + 4));
    USBSerial.printf("rilevatore di tocco a %d Hz\n", motionTapRateHz());
    return;
  }
  if (!strncmp(arg, "int ", 4)) {  // which sensor pin carries the tap events (experiment)
    bool asleep = screen == SCREEN_OFF && wakeMode == WAKE_TAP;
    if (asleep) levelWake(PIN_IMU_INT, false);
    motionTapPin(atoi(arg + 4));
    PIN_IMU_INT = motionTapPinNo() == 1 ? PIN_IMU_INT1 : PIN_IMU_INT2;
    if (asleep) levelWake(PIN_IMU_INT, true);
    USBSerial.printf("eventi del sensore su INT%d (GPIO%d)\n", motionTapPinNo(), PIN_IMU_INT);
    return;
  }
  if (!strncmp(arg, "soglia ", 7)) {  // tap thresholds in thousandths of g: "sveglia soglia 300 150"
    int peak = 0, quiet = 0;
    if (sscanf(arg + 7, "%d %d", &peak, &quiet) == 2 && peak > 0 && quiet > 0) {
      motionTapThreshold(peak / 1000.0f, quiet / 1000.0f);
      USBSerial.printf("soglie del rilevatore: picco %d, quiete %d (millesimi di g)\n", peak, quiet);
    }
    return;
  }
  if (!strncmp(arg, "drdy ", 5)) {  // data-ready pulses on INT2 (experiment)
    motionDrdy(atoi(arg + 5) != 0);
    USBSerial.printf("impulsi dati-pronti %s\n", atoi(arg + 5) ? "accesi" : "spenti");
    return;
  }
  if (!strcmp(arg, "imu")) {  // the sensor's registers, the pins, and the pulses seen since the last call
    static uint32_t lastAt = 0, last[2] = {0, 0};
    char regs[160];
    motionRegs(regs, sizeof(regs));
    uint32_t now = millis(), e0 = imuEdges[0], e1 = imuEdges[1];
    float s = lastAt ? (now - lastAt) / 1000.0f : 0;
    USBSerial.printf("%s | GPIO16=%d GPIO17=%d | fronti: GPIO16 %lu GPIO17 %lu%s\n", regs, digitalRead(PIN_IMU_INT1), digitalRead(PIN_IMU_INT2), (unsigned long)(e0 - last[0]),
                     (unsigned long)(e1 - last[1]), s > 0 ? "" : " (dall'avvio)");
    if (s > 0) USBSerial.printf("  in %.1f s: %.0f/s e %.0f/s\n", s, (e0 - last[0]) / s, (e1 - last[1]) / s);
    lastAt = now;
    last[0] = e0;
    last[1] = e1;
    return;
  }
  WakeMode m = wakeMode;
  if (!strcmp(arg, "tasto")) m = WAKE_KEY;
  else if (!strcmp(arg, "tocco")) m = WAKE_TAP;
  else if (!strcmp(arg, "doppio")) m = WAKE_DOUBLE;
  else if (!strcmp(arg, "schermo")) m = WAKE_TOUCH;
  else if (*arg) {
    USBSerial.println("uso: sveglia [tasto|tocco|doppio|schermo|odr <hz>|soglia <picco> <quiete>|int 1|2|drdy 0|1|imu]");
    return;
  }
  if (m != wakeMode) {
    wakeMode = m;
    Preferences p;
    if (p.begin("madre", false)) {
      p.putUChar("wake", m);
      p.end();
    }
    motionTapWake(tapWakes());
    if (screen == SCREEN_OFF) wakeSources(true);
  }
  USBSerial.printf("sveglia: %s (tasto laterale%s%s%s)\n", wakeModeName(wakeMode), wakeMode == WAKE_TAP ? " o un colpo sul vetro" : "",
                   wakeMode == WAKE_DOUBLE ? " o doppio colpo sul vetro" : "", wakeMode == WAKE_TOUCH ? ", touch sempre in ascolto" : "");
}

static void manageLink(uint32_t ms) {
  int32_t since = (int32_t)(ms - lastActivity);
  bool want = !bleBlocked && (screen != SCREEN_OFF || usbHeld || since < (int32_t)BLE_WINDOW_MS || (linkConnected() && since < (int32_t)(BLE_WINDOW_MS + BLE_SYNC_GRACE_MS)));
  if (want && !linkRunning()) linkStart();
  else if (!want && linkRunning()) linkStop();
}

static void diagStopBle() {
  bleBlocked = true;
  linkStop();
}

static void wake() {
  lastActivity = millis();
  bool wasOff = screen == SCREEN_OFF;
  diagDisturbed();
  if (wasOff) {
    wakeSources(false);
    if (touchHibernated) touchRevive();
    else touchWakeSource(false);
    esp_pm_lock_acquire(awakeLock);
    esp_pm_lock_acquire(fastLock);
    motionFast(true);
    panelPower(true);
    delay(5);
    gfx->reinit();
    lv_obj_invalidate(lv_screen_active());  // the panel's memory was lost: LVGL draws it all again
  }
  if (screen != SCREEN_ON) {
    gfx->setBrightness(BRIGHTNESS_ON);
    screen = SCREEN_ON;
  }
  if (wasOff) {
    think();  // hours may have passed: the right face straight away
    // A word on waking up, now and then: hello when well, a plea when hungry.
    static uint32_t lastGreeting = 0;
    uint32_t now = millis();
    if (!brain.asleep && brain.mood != DORMANT && (!lastGreeting || now - lastGreeting > 30 * 60 * 1000)) {
      lastGreeting = now;
      audioPlay(brain.mood == HUNGRY || brain.mood == STARVING ? SND_HUNGRY : SND_HELLO);
    }
  }
}

static void sleepScreen() {
  gfx->setBrightness(0);
  gfx->displayOff();
  panelPower(false);
  statsUntil = 0;
  motionFast(false);
  screen = SCREEN_OFF;
  if (wakeMode == WAKE_TOUCH) touchWakeSource(true);
  else touchHibernate();
  wakeSources(true);
  esp_pm_lock_release(fastLock);
  esp_pm_lock_release(awakeLock);
}

static void flushCb(lv_display_t *display, const lv_area_t *area, uint8_t *px) {
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, lv_area_get_width(area), lv_area_get_height(area));
  lv_display_flush_ready(display);
}

// CO5300 needs flush areas aligned to even rows; full-width rows keep it simple.
static void rounderCb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  int32_t y1 = area->y1 & ~1;
  int32_t y2 = area->y2 | 1;
  area->x1 = 0;
  area->x2 = (int32_t)screenWidth - 1;
  area->y1 = y1 < 0 ? 0 : y1;
  area->y2 = y2 >= (int32_t)screenHeight ? (int32_t)screenHeight - 1 : y2;
}

// The touch controller only leaves hibernation through its reset pin. The
// line then stays driven high, also through light sleep: left floating
// there it cost about 0.3 mA (measured).
static void touchReset() {
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  gpio_sleep_sel_dis((gpio_num_t)TP_RESET);
  delay(200);
}

static bool displayBegin() {
  if (!gfx->begin()) return false;
  gfx->fillScreen(RGB565_BLACK);

  Wire.begin(IIC_SDA, IIC_SCL);
  touchReset();  // out of hibernation, if a diagnosis left it there
  for (uint8_t attempt = 0; attempt < 5 && !touchReady; attempt++) {
    touchReady = touch->begin();
    if (!touchReady) delay(300);
  }
  if (touchReady) {
    touch->IIC_Write_Device_State(touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE, touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  } else {
    USBSerial.println("touch non disponibile");
  }

  lv_init();
  lv_tick_set_cb(millisCb);
  screenWidth = gfx->width();
  screenHeight = gfx->height();
  size_t bytes = (size_t)screenWidth * LVGL_BUFFER_LINES * lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
  uint8_t *buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) return false;

  lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(disp, rounderCb, LV_EVENT_INVALIDATE_AREA, NULL);
  return true;
}

// ─── State ──────────────────────────────────────────────────────────────────

static void reportStatus(bool ok, const char *err) {
  linkReportStatus(saved.pending, saved.nPending, battery, ok, err);
}

static void think() {
  int64_t now = clockNow();
  brain = now ? petState(saved.refreshes, saved.nRefreshes, now, saved.tz) : PetState();
  if (previewOn && millis() - previewSince > PREVIEW_MAX_MS) previewOn = false;
  if (previewOn) {
    brain.mood = previewMood;
    brain.asleep = previewNight;
  }
  pet.setState(brain.mood, brain.asleep, millis());
  uiSetMeters(segments(brain.satiety), segments(brain.vitality));
}

static void applyState(const std::string &payload) {
  JsonDocument doc;
  if (eventLog) USBSerial.printf("stato ricevuto: %.*s\n", (int)payload.size(), payload.data());
  if (deserializeJson(doc, payload.data(), payload.size())) return reportStatus(false, "json");
  if ((doc["v"] | 0) != MADRE_PROTOCOL) return reportStatus(false, "versione");

  int64_t before = saved.nRefreshes ? saved.refreshes[0] : 0;
  int64_t now = doc["now"] | (int64_t)0;
  clockSet(now);
  saved.tz = doc["tz"] | 0;
  snprintf(saved.name, sizeof(saved.name), "%s", (const char *)(doc["name"] | "Madre"));
  saved.nRefreshes = 0;
  for (JsonVariant t : doc["r"].as<JsonArray>()) {
    if (saved.nRefreshes >= MADRE_MAX_REFRESHES) break;
    saved.refreshes[saved.nRefreshes++] = t.as<int64_t>();
  }
  saved.nPending = 0;  // the watch does not log refreshes
  storeSave(saved);

  // Test panel: a forced state while it is open, and one-off reactions.
  JsonVariant preview = doc["preview"];
  bool wasPreview = previewOn;
  previewOn = preview.is<JsonObject>();
  if (previewOn) {
    static const char *const NAMES[] = {"newborn", "satiated", "active", "calm", "peckish", "hungry", "starving", "dormant"};
    const char *mood = preview["mood"] | "";
    for (int i = 0; i < 8; i++)
      if (!strcmp(mood, NAMES[i])) previewMood = (Mood)i;
    previewNight = preview["night"] | false;
    previewSince = millis();
    wake();
  } else if (wasPreview) {
    wake();  // back to the real state: show it
  }

  // Fed on the phone a moment ago: the meal plays here too.
  int64_t newest = saved.nRefreshes ? saved.refreshes[0] : 0;
  if (newest > before && now - newest < FRESH_REFRESH_S) {
    wake();
    pet.celebrate(millis());
  }
  think();
  const char *act = doc["act"] | "";
  uint32_t ms = millis();
  if (*act) wake();
  if (!strcmp(act, "giggle")) pet.giggle(ms);
  else if (!strcmp(act, "annoy")) pet.annoy(ms);
  else if (!strcmp(act, "grumpy")) pet.grumpy(ms);
  else if (!strcmp(act, "feed")) pet.celebrate(ms);
  else if (!strcmp(act, "quirk")) pet.quirkNow(ms);
  reportStatus(true, "");
  USBSerial.printf("stato dal telefono: %s, %u rinfreschi%s%s%s\n", saved.name, saved.nRefreshes, previewOn ? ", prova " : "", previewOn ? (const char *)(preview["mood"] | "") : "",
                   *act ? " + azione" : "");
}

// ─── Input ──────────────────────────────────────────────────────────────────

static void recoverI2C() {
  i2cRecoveries++;
  if (eventLog) USBSerial.printf("bus I2C bloccato: lo riavvio (%d)\n", i2cRecoveries);
  Wire.end();
  delay(5);
  Wire.begin(IIC_SDA, IIC_SCL);
  touchReady = touch->begin();
  if (touchReady)
    touch->IIC_Write_Device_State(touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE, touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  touchFails = 0;
  // begin() attached the interrupt again, on a falling edge.
  if (screen == SCREEN_OFF) touchWakeSource(true);
}

// Read only when the controller has something to say (its interrupt) or a
// finger is down (to see it lift): otherwise it is in monitor mode and every
// read fails, which used to look like a stuck bus.
static void readTouch(uint32_t ms) {
  if (!touchIrq && !touchDown) return;
  touchIrq = false;
  if (!touchReady) {
    recoverI2C();
    if (!touchReady) return;
  }
  double fingers = touch->IIC_Read_Device_Value(touch->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (fingers < 0) {
    if (++touchFails < 3) return;
    // Silent while a finger was down: it went back to monitor mode, so the
    // finger is gone. Restart the bus only if it keeps failing.
    fingers = 0;
    if (touchFails >= 10) recoverI2C();
  } else {
    touchFails = 0;
  }
  bool down = fingers > 0;
  if (down) {
    touchX = touch->IIC_Read_Device_Value(touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    touchY = touch->IIC_Read_Device_Value(touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  }
  if (down && !touchDown) {
    // The first touch on a dark screen only wakes it (a fading one still plays).
    swallowTouch = screen == SCREEN_OFF;
    wake();
    touchSince = ms;
    touchStartX = touchX;
    touchStartY = touchY;
    if (eventLog) USBSerial.printf("%7u tocco giù %d,%d%s\n", ms, touchX, touchY, swallowTouch ? " (solo risveglio)" : "");
    if (!swallowTouch) pet.touchDown(touchX, touchY - STAGE_Y, ms);
  } else if (down) {
    lastActivity = ms;
    // Holding still for a couple of seconds shows the stats.
    if (ms - touchSince >= STATS_HOLD_MS) statsUntil = ms + STATS_LINGER_MS;
  } else if (touchDown) {
    if (eventLog)
      USBSerial.printf("%7u tocco su dopo %u ms, spostamento %d,%d\n", ms, ms - touchSince, touchX - touchStartX, touchY - touchStartY);
    if (!swallowTouch) pet.touchUp(touchX, touchY - STAGE_Y, ms);
    swallowTouch = false;
  }
  touchDown = down;
}

// A sudden shake startles it (screen on only: nobody reads the accelerometer
// while the screen sleeps). Shake
// "energy" builds up from how far the total acceleration strays from gravity
// and leaks away: one hard jolt or a brisk shake sets it off. Turning the
// wrist only changes the direction of gravity, not its size, so it never counts.
static float lastX = 0, lastY = 0, lastZ = 1, gravity = 1, shakeEnergy = 0;
static uint32_t shakeCooldown = 0;
static bool motionLog = false;

static void readMotion(uint32_t ms) {
  float x, y, z;
  if (!motionRead(x, y, z)) return;
  float jerk = fabsf(x - lastX) + fabsf(y - lastY) + fabsf(z - lastZ);
  lastX = x;
  lastY = y;
  lastZ = z;
  float mag = sqrtf(x * x + y * y + z * z);
  float stray = fabsf(mag - gravity);
  if (stray < 0.15f) gravity += (mag - gravity) * 0.02f;  // the sensor reads ~1.08 g at rest
  shakeEnergy = shakeEnergy * 0.75f + fmaxf(0, stray - 0.3f);
  if (eventLog && shakeEnergy > 0.15f) USBSerial.printf("%7u energia %.2f (scarto %.2f g, rotazione %.2f)\n", ms, shakeEnergy, stray, jerk);
  if (motionLog) USBSerial.printf("acc %.4f %.4f %.4f  scatto %.2f  energia %.2f\n", x, y, z, jerk, shakeEnergy);
  if (shakeEnergy > 0.6f && (int32_t)(ms - shakeCooldown) >= 0) {
    if (eventLog) USBSerial.printf("%7u scossone!\n", ms);
    shakeEnergy = 0;
    shakeCooldown = ms + 2500;
    wake();
    pet.shake(ms);
  }
}

// ─── Main ───────────────────────────────────────────────────────────────────

// Frame timing, for "stato".
static uint32_t frames = 0, renderMicros = 0, pushMicros = 0;

static void drawStage() {
  pet.frame(frame);
  prepareStage(frame);
  for (int y = 0; y < STAGE_H; y += STRIP_ROWS) {
    uint32_t t1 = micros();
    renderStage(y, STRIP_ROWS, strip);
    uint32_t t2 = micros();
    renderMicros += t2 - t1;
    gfx->draw16bitBeRGBBitmap(0, STAGE_Y + y, strip, STAGE_W, STRIP_ROWS);
    pushMicros += micros() - t2;
  }
  frames++;
}

// Why the watch last started, kept in NVS with a boot counter ("stato").
static uint32_t boots = 0;
static uint8_t lastReason = 0;
static int64_t lastBootAt = 0;

static const char *reasonName(uint8_t r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON: return "accensione";
    case ESP_RST_SW: return "riavvio software";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT: return "BLOCCO (interrupt)";
    case ESP_RST_TASK_WDT: return "BLOCCO (programma)";
    case ESP_RST_WDT: return "BLOCCO (sistema)";
    case ESP_RST_BROWNOUT: return "calo di tensione";
    case ESP_RST_USB: return "USB / caricamento";
    case ESP_RST_JTAG: return "USB / caricamento";
    case ESP_RST_DEEPSLEEP: return "risveglio";
    default: return "altro";
  }
}

static void noteBoot() {
  Preferences p;
  if (!p.begin("madre", false)) return;
  boots = p.getUInt("boots", 0) + 1;
  p.putUInt("boots", boots);
  lastReason = (uint8_t)esp_reset_reason();
  lastBootAt = clockNow();
  p.end();
}

static void notifyLoop() {
  if (loopTask) xTaskNotifyGive(loopTask);
}

// ─── Power diagnosis (diag.h) ───────────────────────────────────────────────
// Each knob on = as in normal sleep, off = that consumer off.

// Asleep in monitor mode the controller does not answer on I2C, so both
// sides start from a reset (awake): then hibernate, or back to monitor.
static void knobTouch(bool on) {
  touchReset();
  touchReady = touch->begin();
  bool ok = touchReady && touchSetMode(on ? touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR : touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_HIBERNATE, 4);
  if (eventLog) USBSerial.printf("touch %s: %s\n", on ? "monitor" : "ibernato", ok ? "ordine accettato" : "ordine RIFIUTATO");
  if (!on) touchReady = false;
  if (screen == SCREEN_OFF) touchWakeSource(true);
}

// Off = the touch controller held in reset; on = out of it, in monitor mode.
static void knobTouchReset(bool on) {
  if (!on) {
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    touchReady = false;
    return;
  }
  knobTouch(true);
}

// On = the accelerometer's tap detector at its rate (the normal sleep when
// the watch wakes on a double tap); off = plain 21 Hz low-power sampling.
static void knobImu(bool on) {
  motionTapWake(on);
}

// "touch": on = monitor mode (the old sleep), off = hibernated (the new one).
static const Knob KNOBS[] = {{"tp-reset", knobTouchReset}, {"touch", knobTouch}, {"imu", knobImu}};

// Knobs measured with the chip asleep: how the chip and its pins are left in
// light sleep. on = as it is now, off = the candidate saving.
static void knobAmp(bool on) {  // the speaker amplifier's enable pin: held low, or kept driven low through sleep
  const gpio_num_t g = GPIO_NUM_6;
  if (on) {
    digitalWrite(g, LOW);
    gpio_sleep_sel_en(g);
    gpio_hold_en(g);
  } else {
    gpio_hold_dis(g);
    pinMode(g, OUTPUT);
    digitalWrite(g, LOW);
    gpio_sleep_sel_dis(g);
  }
}
static void domainOff(esp_sleep_pd_domain_t d, bool on) {
  esp_sleep_pd_config(d, on ? ESP_PD_OPTION_AUTO : ESP_PD_OPTION_OFF);
}
static void knobXtalOff(bool on) { domainOff(ESP_PD_DOMAIN_XTAL, on); }
static void knobRcFastOff(bool on) { domainOff(ESP_PD_DOMAIN_RC_FAST, on); }
static void knobModemOff(bool on) { domainOff(ESP_PD_DOMAIN_MODEM, on); }
// The deep-sleep test: the pins kept as light sleep leaves them (display lines
// and resets driven, amplifier off, I2S pulled down, the wake inputs).
static constexpr int DEEP_HELD[] = {LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, LCD_SCLK, LCD_CS, LCD_RESET, TP_RESET, 6, 16, 17, 18, 19, 20, 21, 22, 23};
static void prepareDeepSleep() {
  for (int pin : DEEP_HELD) gpio_hold_en((gpio_num_t)pin);  // on the C6 each pad holds through deep sleep by itself
}

static const Knob SLEEP_KNOBS[] = {{"ampli", knobAmp}, {"xtal-off", knobXtalOff}, {"rcfast-off", knobRcFastOff}, {"modem-off", knobModemOff}};

void setup() {
  USBSerial.setRxBufferSize(4096);  // room for a test "carica <json>"
  USBSerial.begin(115200);
  powerBegin();
  // Back from a deep sleep: the pins were held as in light sleep; let them go.
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    for (int pin : DEEP_HELD) gpio_hold_dis((gpio_num_t)pin);
  }
  hwBegin();  // first: it powers the audio rail the shared I2C bus needs
  diagEarlyBoot();  // a deep-sleep measurement reads the battery now, before the panel powers up
  // The display lines keep their awake setup through light sleep instead of
  // floating into the panel's chip: 0.6 mA less (measured).
  for (int pin : {LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, LCD_SCLK, LCD_CS, LCD_RESET}) gpio_sleep_sel_dis((gpio_num_t)pin);
  // The power manager keeps its rails through a restart: if the screen was
  // asleep then, the panel has no power and would stay black until the
  // first wake.
  panelPower(true);
  delay(5);
  if (!displayBegin()) {
    USBSerial.println("display non disponibile");
    return;
  }
  noteBoot();
  enableLoopWDT();
  storeLoad(saved);
  uiBegin();
  audioBegin();
  wakeBegin();  // the side key and the accelerometer as wake sources
  pet.begin(millis(), esp_random());
  battery = batteryPercent();
  think();
  linkStart();
  diagSleepKnobs(SLEEP_KNOBS, sizeof(SLEEP_KNOBS) / sizeof(SLEEP_KNOBS[0]));
  diagDeepHook(prepareDeepSleep);
  diagBegin(KNOBS, sizeof(KNOBS) / sizeof(KNOBS[0]), diagStopBle);
  reportStatus(true, "");
  esp_sleep_enable_gpio_wakeup();
  loopTask = xTaskGetCurrentTaskHandle();
  linkOnPayload(notifyLoop);
  lastActivity = millis();
  USBSerial.printf("MADRE %s pronto, audio %s, heap libero %u\n", MADRE_FW_VERSION, audioReady() ? "ok" : "non disponibile", (unsigned)ESP.getFreeHeap());
}

// Debug over USB (115200):
//   stato            one-line report and frame timing
//   carica <json>    applies a state exactly as if it came over BLE
//   tocca            a tap on the eyes
//   scuoti           a shake
//   pasto            the meal animation
//   premi            shows the stats as a long press would
//   suono <0..7>     plays one of the sounds
//   batteria         the battery log (every 5 minutes) and the measured drain
//   pm               time spent awake / in light sleep since boot, and the locks
//   avvii            boot counter, why the watch last restarted, I2C recoveries
//   moto             toggles the accelerometer log
//   vbat             battery voltage, charger state and charge current
//   calibra [n]      with USB in: battery resistance from the charger (diag.h)
//   diagnosi [via [min] | stop]   power diagnosis on battery: results, arm, cancel
//   manopola <nome> 0|1           one diagnosis knob by hand (tp-reset, touch, imu)
//   sveglia [tasto|tocco|doppio|schermo]   how the watch wakes from a dark screen (saved); odr/soglia/int/drdy/imu: tuning
//   reg <r> [v], i2c <a> <r> [v]  AXP2101 / any I2C register, hex
static void serialCommands() {
  if (!USBSerial.available()) return;
  String cmd = USBSerial.readStringUntil('\n');
  cmd.trim();
  uint32_t ms = millis();
  if (diagCommand(cmd.c_str())) return;
  if (cmd == "stato") {
    static uint32_t lastFrames = 0, lastAt = 0, lastRender = 0, lastPush = 0;
    uint32_t n = frames - lastFrames;
    if (n)
      USBSerial.printf("fotogrammi: %.1f al secondo; calcolo %.1f ms, invio %.1f ms ciascuno\n", n * 1000.0f / (ms - lastAt),
                       (renderMicros - lastRender) / 1000.0f / n, (pushMicros - lastPush) / 1000.0f / n);
    lastFrames = frames;
    lastAt = ms;
    lastRender = renderMicros;
    lastPush = pushMicros;
    USBSerial.print("ciclo più lento:");
    for (int p = 0; p < PARTS; p++) {
      USBSerial.printf(" %s %.1f ms", PART_NAMES[p], partMax[p] / 1000.0f);
      partMax[p] = 0;
    }
    USBSerial.println();
    USBSerial.printf("MADRE %s, %s, umore %d, sonno %d, ore %.1f, rinfreschi %u, ora %lld, tz %d, collegato %d, batteria %d, audio %d, heap %u\n", MADRE_FW_VERSION,
                     saved.name, brain.mood, brain.asleep, brain.hours, saved.nRefreshes, (long long)clockNow(), saved.tz, linkConnected(), battery,
                     audioReady(), (unsigned)ESP.getFreeHeap());
  } else if (cmd.startsWith("carica ")) {
    applyState(std::string(cmd.c_str() + 7));
  } else if (cmd == "tocca") {
    wake();
    pet.touchDown(205, 156, ms);
    pet.touchUp(205, 156, ms);
  } else if (cmd == "scuoti") {
    wake();
    pet.shake(ms);
  } else if (cmd == "pasto") {
    wake();
    pet.celebrate(ms);
  } else if (cmd == "premi") {
    wake();
    statsUntil = ms + STATS_LINGER_MS;
  } else if (cmd.startsWith("suono ")) {
    audioPlay((Sound)cmd.substring(6).toInt());
  } else if (cmd == "linee") {
    char report[400];
    railsReport(report, sizeof(report));
    USBSerial.println(report);
  } else if (cmd == "avvii") {
    USBSerial.printf("avvii %u; ultimo: %s (codice %u), ora unix %lld; recuperi del bus I2C %d\n", boots, reasonName(lastReason), lastReason,
                     (long long)lastBootAt, i2cRecoveries);
  } else if (cmd == "sveglia" || cmd.startsWith("sveglia ")) {
    wakeCommand(cmd.length() > 8 ? cmd.c_str() + 8 : "");
  } else if (cmd == "pm") {
    // How long the chip spent in each power mode since boot, and the locks.
    static char dump[2048];
    FILE *f = fmemopen(dump, sizeof(dump) - 1, "w");
    if (f) {
      esp_pm_dump_locks(f);
      long n = ftell(f);
      fclose(f);
      dump[n < 0 ? 0 : n] = 0;
      USBSerial.print(dump);
    }
  } else if (cmd == "batteria") {
    printBatteryLog();
  } else if (cmd.startsWith("bleciclo ")) {
    // Stress test of the BLE window: the stack on and off n times, heap as it goes.
    int n = cmd.substring(9).toInt();
    uint32_t heap0 = ESP.getFreeHeap();
    for (int i = 1; i <= n; i++) {
      linkStart();
      delay(250);
      linkStop();
      delay(50);
      feedLoopWDT();
      if (i % 25 == 0 || i == n) USBSerial.printf("ciclo %d: heap %u (%+d)\n", i, (unsigned)ESP.getFreeHeap(), (int)ESP.getFreeHeap() - (int)heap0);
    }
  } else if (cmd == "moto") {
    motionLog = !motionLog;
  } else if (cmd == "log") {
    eventLog = !eventLog;
    USBSerial.printf("registro eventi %s\n", eventLog ? "acceso" : "spento");
  }
}

void loop() {
  static uint32_t lastFrame = 0, lastThink = 0, lastBattery = 0;
  uint32_t ms = millis();

  powerUsb();
  watchCharger(ms);
  // Screen-on time and the battery log.
  static uint32_t lastMs = ms;
  if (screen != SCREEN_OFF) screenOnMs += ms - lastMs;
  lastMs = ms;
  if (ms - lastLogAt >= 5 * 60 * 1000 || !logCount) {
    lastLogAt = ms;
    logBattery(ms);
  }
  serialCommands();
  manageLink(ms);
  std::string payload;
  if (linkTakePayload(payload)) applyState(payload);
  char event[80];
  while (linkTakeEvent(event, sizeof(event)))
    if (eventLog) USBSerial.printf("%7u bluetooth: %s\n", ms, event);

  if (screen == SCREEN_OFF) {
    // A power diagnosis may be running (diag.h).
    diagTick(ms, usbHeld);
    // Asleep: only what can wake it, at a gentle pace; the idle task halts
    // the CPU in between, and a touch wakes it at once (touchWakeSource).
    if (touchIrq) {
      readTouch(ms);
      if (screen == SCREEN_OFF) {
        if (eventLog) USBSerial.printf("%7u tocco (interrupt) a schermo spento\n", ms);
        swallowTouch = true;  // a finger still down is the waking touch
        touchDown = true;
        wake();
      }
    }
    wakePinsTick(ms);
    if (ms - lastThink >= ASLEEP_THINK_MS) {
      lastThink = ms;
      think();
    }
    if (ms - lastBattery >= 60000) {
      lastBattery = ms;
      battery = batteryPercent();
    }
    if (screen == SCREEN_OFF) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(usbHeld ? ASLEEP_WAIT_USB_MS : ASLEEP_WAIT_MS));
      return;
    }
  }

  uint32_t t = micros(), loopStart = t;
  touchReviveTick(ms);
  wakePinsTick(ms);
  readTouch(ms);
  timed(P_TOUCH, t);
  t = micros();
  readMotion(ms);
  timed(P_MOTION, t);
  if (ms - lastThink >= 1000) {
    lastThink = ms;
    think();
  }
  if (ms - lastBattery >= 60000) {
    lastBattery = ms;
    battery = batteryPercent();
  }
  bool stats = statsUntil && (int32_t)(statsUntil - ms) > 0;
  if (stats != statsOn) {
    statsOn = stats;
    uiShowStats(stats);
  }

  // Screen: on while in use, then a short fade and the panel sleeps.
  // Signed: wake() stamps lastActivity after this loop's `ms` (the panel
  // takes a moment to power up), so the difference can be negative.
  if (previewOn) lastActivity = ms;  // testing from the app: stay on
  if (screen == SCREEN_ON && (int32_t)(ms - lastActivity) > (int32_t)AWAKE_MS) {
    screen = SCREEN_FADING;
    fadeStart = ms;
  }
  if (screen == SCREEN_FADING) {
    float k = (ms - fadeStart) / (float)FADE_MS;
    if (k >= 1) {
      sleepScreen();
      return;
    }
    gfx->setBrightness((uint8_t)(BRIGHTNESS_ON * (1 - k)));
  }

  pet.update(ms);
  if (pet.takeMuteToggle()) {
    bool mute = !audioMuted();
    audioSetMuted(mute);
    uiShowSound(!mute);
    pet.soundToggled(mute, ms);
    if (eventLog) USBSerial.printf("%7u suoni %s\n", ms, mute ? "spenti" : "accesi");
  }
  int sound = pet.takeSound();
  if (sound >= 0) audioPlay((Sound)sound);
  t = micros();
  lv_timer_handler();
  timed(P_LVGL, t);
  if (ms - lastFrame >= FRAME_MS) {
    lastFrame = ms;
    t = micros();
    drawStage();
    timed(P_STAGE, t);
  }
  timed(P_LOOP, loopStart);
  delay(1);
}
