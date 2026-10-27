#include "diag.h"
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>
#include "HWCDC.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "hw.h"

extern HWCDC USBSerial;

static const Knob *knobs = nullptr, *sleepKnobs = nullptr;
static int nKnobs = 0, nSleepKnobs = 0;

void diagSleepKnobs(const Knob *k, int count) {
  sleepKnobs = k;
  nSleepKnobs = count;
}
static void (*bleStop)() = nullptr;
// The diagnosis keeps the chip awake (and steady) while it measures; only the
// sleep knobs let it go.
static esp_pm_lock_handle_t awake, fast;
static bool busy = false;

// ─── Results (NVS) ──────────────────────────────────────────────────────────

// kind 's' (step): a = mean jump in mV (off − on), b = standard error, c = mA.
// kind 'b' (block): a = rest voltage at the start, b = at the end, c = mV/h.
struct Result {
  char name[14];
  char kind;
  uint8_t fails;
  uint16_t n;
  float a, b, c;
};
static constexpr int MAX_RESULTS = 20;
static Result results[MAX_RESULTS];
static int nResults = 0;
static float resistance = 0;  // battery + path, ohm (mV per mA), from "calibra"

enum Phase : uint8_t { P_NONE, P_ARMED, P_RUNNING, P_DONE };
static Phase phase = P_NONE;
static uint16_t blockMinutes = 30;
static int8_t resumeBlock = -1;  // block to start after a planned restart
static bool stepsOnly = false;   // "diagnosi passi": every knob once, no blocks
static bool floorRun = false;    // "diagnosi suolo": asleep / awake / asleep, for the absolute current
static bool deepRun = false;     // "diagnosi profondo": light sleep block, then deep sleep
static void (*deepPrepare)() = nullptr;
static float deepV0 = 0;         // the voltage going into deep sleep, and when (RTC seconds)
static int64_t deepT0 = 0;

static void save() {
  Preferences p;
  if (!p.begin("diag", false)) return;
  p.putBytes("res", results, sizeof(Result) * nResults);
  p.putInt("n", nResults);
  p.putFloat("rint", resistance);
  p.putUChar("phase", phase);
  p.putUShort("min", blockMinutes);
  p.putChar("resume", resumeBlock);
  p.putBool("steps", stepsOnly);
  p.putBool("floor", floorRun);
  p.putBool("deep", deepRun);
  p.putFloat("dv0", deepV0);
  p.putLong64("dt0", deepT0);
  p.end();
}

static void load() {
  Preferences p;
  if (!p.begin("diag", true)) return;
  nResults = p.getInt("n", 0);
  if (nResults < 0 || nResults > MAX_RESULTS) nResults = 0;
  if (nResults) p.getBytes("res", results, sizeof(Result) * nResults);
  resistance = p.getFloat("rint", 0);
  phase = (Phase)p.getUChar("phase", P_NONE);
  blockMinutes = p.getUShort("min", 30);
  resumeBlock = p.getChar("resume", -1);
  stepsOnly = p.getBool("steps", false);
  floorRun = p.getBool("floor", false);
  deepRun = p.getBool("deep", false);
  deepV0 = p.getFloat("dv0", 0);
  deepT0 = p.getLong64("dt0", 0);
  p.end();
}

static void addResult(const Result &r) {
  for (int i = 0; i < nResults; i++)
    if (!strcmp(results[i].name, r.name)) {
      results[i] = r;
      save();
      return;
    }
  if (nResults < MAX_RESULTS) results[nResults++] = r;
  save();
}

// ─── Measuring ──────────────────────────────────────────────────────────────

static void wait(uint32_t ms) {
  if (!busy) {
    delay(ms);
    return;
  }
  uint32_t t = millis();
  while (millis() - t < ms) {
  }
}

// Mean of n battery readings, one every `gap` ms (asleep in between unless a
// lock or the busy knob keeps the chip up).
static float sample(int n, uint32_t gap, int &fails) {
  float sum = 0;
  int got = 0;
  for (int i = 0; i < n; i++) {
    wait(gap);
    int v = batteryMillivolts();
    if (v > 2500 && v < 4600) {
      sum += v;
      got++;
    } else {
      fails++;
    }
  }
  return got ? sum / got : NAN;
}

// Voltage at rest, the same way every time: awake, settled, 64 readings.
static float restMillivolts() {
  esp_pm_lock_acquire(awake);
  delay(1500);
  int fails = 0;
  float v = sample(64, 20, fails);
  esp_pm_lock_release(awake);
  return v;
}

// The same, with the CPU at 160 MHz: the state the chip boots in, so the
// readings before and after a deep sleep carry the same load.
static float restMillivoltsFast() {
  esp_pm_lock_acquire(fast);
  float v = restMillivolts();
  esp_pm_lock_release(fast);
  return v;
}

// One consumer, `cycles` times off and on. Each "off" is compared with the
// mean of the "on" before and after it, so a slow drift cancels out.
static Result step(const char *name, void (*set)(bool), int cycles, uint32_t SETTLE_MS = 600) {
  static constexpr uint32_t GAP_MS = 40;
  static constexpr int N = 10;
  Result r = {};
  snprintf(r.name, sizeof(r.name), "%s", name);
  r.kind = 's';
  int fails = 0;
  set(true);
  wait(SETTLE_MS);
  float prevOn = sample(N, GAP_MS, fails);
  double sum = 0, sum2 = 0;
  int m = 0;
  for (int c = 0; c < cycles; c++) {
    set(false);
    wait(SETTLE_MS);
    float off = sample(N, GAP_MS, fails);
    set(true);
    wait(SETTLE_MS);
    float on = sample(N, GAP_MS, fails);
    float d = off - (prevOn + on) / 2;
    prevOn = on;
    if (!isnan(d)) {
      sum += d;
      sum2 += (double)d * d;
      m++;
    }
    feedLoopWDT();
  }
  r.n = m;
  r.fails = fails > 255 ? 255 : fails;
  r.a = m ? sum / m : NAN;
  r.b = m > 1 ? sqrt(fmax(0, sum2 / m - (sum / m) * (sum / m)) / (m - 1)) : NAN;
  r.c = resistance > 0 ? r.a / resistance : NAN;
  return r;
}

// Knobs of the chip itself.
static void knobAwake(bool on) {  // on = kept awake at 40 MHz, off = light sleep allowed
  if (on) esp_pm_lock_acquire(awake);
  else esp_pm_lock_release(awake);
}
static void knobCpu(bool on) {  // on = spinning at 160 MHz, off = idle at 40 MHz
  busy = on;
  if (on) esp_pm_lock_acquire(fast);
  else esp_pm_lock_release(fast);
}
static void knobXtal(bool on) {  // main crystal kept on in light sleep (what BLE asks for)
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, on ? ESP_PD_OPTION_ON : ESP_PD_OPTION_OFF);
}
static void knobCharger(bool on) {
  chargerEnable(on);
}

static void stepAndSave(const char *name, void (*set)(bool), int cycles, uint32_t settleMs = 0) {
  USBSerial.printf("passo %s...\n", name);
  Result r = step(name, set, settleMs ? cycles / 2 : cycles, settleMs ? settleMs : 600);
  addResult(r);
}

// The series of steps. Called with the screen off and the chip awake (lock);
// the sleep knobs release that lock around their own step. The other knobs
// are for "manopola" by hand: their steps were measured already.
static void runSteps(bool withBle) {
  static constexpr int CYCLES = 25;
  if (withBle) {
    stepAndSave("sonno", knobAwake, CYCLES);  // ends awake: knob left on = lock held
    stepAndSave("cpu160", knobCpu, CYCLES);
    knobCpu(false);
    if (stepsOnly) {
      for (int i = 0; i < nKnobs; i++) stepAndSave(knobs[i].name, knobs[i].set, CYCLES, knobs[i].settleMs);
      // These with the chip asleep between readings: the first reading after
      // each wake still shows the sleeping current (the "sonno" step relies
      // on the same).
      esp_pm_lock_release(awake);
      for (int i = 0; i < nSleepKnobs; i++) stepAndSave(sleepKnobs[i].name, sleepKnobs[i].set, CYCLES, sleepKnobs[i].settleMs);
      esp_pm_lock_acquire(awake);
    }
  } else {
    stepAndSave("sonno-noble", knobAwake, CYCLES);
    esp_pm_lock_release(awake);
    stepAndSave("xtal", knobXtal, CYCLES);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_AUTO);
    esp_pm_lock_acquire(awake);
  }
  esp_pm_lock_release(awake);
}

// ─── The scheduled run on battery ───────────────────────────────────────────

// N1 and N2 bracket C (BLE off) so the battery's own curve cancels. BLE only
// comes back with a restart, so the run saves where it is and resumes with
// N2 after it. Touch keeps working throughout.
static const char *const BLOCKS[] = {"N1-normale", "C-senza-ble", "N2-normale"};
static constexpr int N_BLOCKS = sizeof(BLOCKS) / sizeof(BLOCKS[0]);
static int block = -1;
static uint32_t blockStart = 0, unpluggedAt = 0;
static float blockVolts = 0;

static void startBlock(int b) {
  block = b;
  blockVolts = restMillivolts();
  blockStart = millis();
  USBSerial.printf("blocco %s: %.0f mV\n", BLOCKS[b], blockVolts);
}

static void finish(bool aborted, bool bleDown) {
  phase = P_DONE;
  floorRun = false;
  block = resumeBlock = -1;
  save();
  USBSerial.printf("diagnosi %s\n", aborted ? "interrotta (USB)" : "finita");
  if (bleDown) esp_restart();  // BLE comes back only with a restart
}

// ─── The floor run: absolute standby current ───────────────────────────────

static const char *const FLOOR_BLOCKS[] = {"F1-sonno", "F2-sveglio", "F3-sonno"};
static constexpr int N_FLOOR = 3;
// The cell's voltage keeps relaxing for minutes after a change of load: the
// slope is timed from this far into each block (40 % of it, 8 minutes at least).
static uint32_t floorSettleMs() {
  uint32_t ms = (uint32_t)blockMinutes * 60 * 1000 * 4 / 10;
  return ms < 8 * 60 * 1000 ? 8 * 60 * 1000 : ms;
}
static bool floorSettled = false;
static uint32_t floorV1At = 0;
static float floorV1 = 0;
static bool floorAwakeHeld = false;

static void floorAwake(bool on) {
  if (on && !floorAwakeHeld) esp_pm_lock_acquire(awake);
  if (!on && floorAwakeHeld) esp_pm_lock_release(awake);
  floorAwakeHeld = on;
}

static void startFloorBlock(int b) {
  floorAwake(false);
  block = b;
  blockStart = millis();
  floorSettled = false;
  floorAwake(b == 1);  // F2: held awake at 40 MHz, the known extra load
  USBSerial.printf("blocco %s\n", FLOOR_BLOCKS[b]);
}

// ─── The deep run ───────────────────────────────────────────────────────────
// Settle 10 min, light sleep 25 min (reference), deep sleep `blockMinutes`.
static constexpr uint32_t DEEP_SETTLE_MS = 10 * 60 * 1000;
static constexpr uint32_t DEEP_LIGHT_MS = 25 * 60 * 1000;
static int deepStage = 0;  // 0 settle, 1 light block
static float deepLightV0 = 0;
static uint32_t deepLightAt = 0;

static void deepTick(uint32_t ms, bool usb) {
  if (usb) {
    finish(true, false);
    deepRun = false;
    save();
    return;
  }
  uint32_t elapsed = ms - blockStart;
  if (deepStage == 0) {
    if (elapsed < DEEP_SETTLE_MS) return;
    deepLightV0 = restMillivoltsFast();
    deepLightAt = millis();
    deepStage = 1;
    USBSerial.printf("profondo: blocco di riferimento da %.1f mV\n", deepLightV0);
    return;
  }
  if (millis() - deepLightAt < DEEP_LIGHT_MS) return;
  float v = restMillivoltsFast();
  Result r = {};
  snprintf(r.name, sizeof(r.name), "L-leggero");
  r.kind = 'b';
  r.n = DEEP_LIGHT_MS / 60000;
  r.a = deepLightV0;
  r.b = v;
  r.c = (deepLightV0 - v) * 3600000.0f / (float)(millis() - deepLightAt);
  addResult(r);
  deepV0 = v;
  deepT0 = clockNow();
  save();  // phase RUNNING and deepRun: the boot after the deep sleep finishes the job
  USBSerial.printf("profondo: %.1f mV/h in sonno leggero; ora sonno profondo per %u min\n", r.c, blockMinutes);
  USBSerial.flush();
  if (deepPrepare) deepPrepare();
  esp_sleep_enable_timer_wakeup((uint64_t)blockMinutes * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

void diagDeepHook(void (*prepare)()) {
  deepPrepare = prepare;
}

void diagEarlyBoot() {
  load();
  if (!(phase == P_RUNNING && deepRun)) return;
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || !deepT0) {
    deepRun = false;
    phase = P_DONE;
    save();
    return;
  }
  if (!awake) esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "diag", &awake);
  if (!fast) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "diag160", &fast);
  float v = restMillivoltsFast();
  int64_t secs = clockNow() - deepT0;
  Result r = {};
  snprintf(r.name, sizeof(r.name), "D-profondo");
  r.kind = 'b';
  r.n = secs > 0 ? (uint16_t)(secs / 60) : 0;
  r.a = deepV0;
  r.b = v;
  r.c = secs > 60 ? (deepV0 - v) * 3600.0f / (float)secs : NAN;
  addResult(r);
  deepRun = false;
  phase = P_DONE;
  save();
}

void diagDisturbed() {
  if (phase == P_RUNNING && floorRun && block >= 0) {
    USBSerial.printf("blocco %s disturbato: riparte\n", FLOOR_BLOCKS[block]);
    startFloorBlock(block);
  }
}

static void floorTick(uint32_t ms, bool usb) {
  if (usb) {
    floorAwake(false);
    finish(true, false);
    return;
  }
  uint32_t elapsed = ms - blockStart;
  if (!floorSettled) {
    if (elapsed < floorSettleMs()) return;
    floorV1 = restMillivolts();
    floorV1At = millis();
    floorSettled = true;
    return;
  }
  if (elapsed < (uint32_t)blockMinutes * 60 * 1000) return;
  float v2 = restMillivolts();
  uint32_t now = millis();
  Result r = {};
  snprintf(r.name, sizeof(r.name), "%s", FLOOR_BLOCKS[block]);
  r.kind = 'b';
  r.n = blockMinutes;
  r.a = floorV1;
  r.b = v2;
  r.c = (floorV1 - v2) * 3600000.0f / (float)(now - floorV1At);  // mV/h over the timed window
  addResult(r);
  USBSerial.printf("blocco %s: %.1f -> %.1f mV, %.1f mV/h\n", FLOOR_BLOCKS[block], floorV1, v2, r.c);
  if (block + 1 == N_FLOOR) {
    floorAwake(false);
    finish(false, false);
    return;
  }
  startFloorBlock(block + 1);
}

DiagMode diagTick(uint32_t ms, bool usb) {
  if (phase == P_ARMED || resumeBlock >= 0) {
    if (usb) {
      unpluggedAt = 0;
      return DIAG_IDLE;
    }
    if (!unpluggedAt) unpluggedAt = ms ? ms : 1;
    // Give the screen time to go dark and the battery to settle.
    if (ms - unpluggedAt < 90 * 1000) return DIAG_IDLE;
    if (resumeBlock >= 0) {
      int b = resumeBlock;
      resumeBlock = -1;
      startBlock(b);
      return DIAG_NORMAL;
    }
    if (deepRun) {  // keeps earlier results: they are the comparison
      phase = P_RUNNING;
      deepStage = 0;
      block = 0;
      blockStart = ms;
      save();
      USBSerial.println("profondo: assestamento");
      return DIAG_NORMAL;
    }
    phase = P_RUNNING;
    nResults = 0;
    for (int i = 0; i < MAX_RESULTS; i++) results[i] = Result();
    save();
    runSteps(true);  // a floor run does the steps first: "sonno" is its calibration, the sleep knobs a hunt
    if (floorRun) {
      startFloorBlock(0);
      return DIAG_NORMAL;
    }
    if (stepsOnly) {
      finish(false, false);
      return DIAG_IDLE;
    }
    startBlock(0);
    return DIAG_NORMAL;
  }
  if (phase != P_RUNNING || block < 0) return DIAG_IDLE;
  if (floorRun) {
    floorTick(ms, usb);
    return DIAG_NORMAL;
  }
  if (deepRun) {
    deepTick(ms, usb);
    return DIAG_NORMAL;
  }
  if (usb) {
    finish(true, block == 1);
    return DIAG_IDLE;
  }
  if (millis() - blockStart < (uint32_t)blockMinutes * 60 * 1000) return DIAG_NORMAL;
  float v = restMillivolts();
  Result r = {};
  snprintf(r.name, sizeof(r.name), "%s", BLOCKS[block]);
  r.kind = 'b';
  r.n = blockMinutes;
  r.a = blockVolts;
  r.b = v;
  r.c = (blockVolts - v) * 60.0f / blockMinutes;
  addResult(r);
  if (block + 1 == N_BLOCKS) {
    finish(false, false);
    return DIAG_IDLE;
  }
  if (block == 0) {
    // C: BLE off entirely, then what it cost asleep (the crystal).
    bleStop();
    runSteps(false);
    startBlock(1);
    return DIAG_NORMAL;
  }
  // After C: restart to get BLE back, then N2.
  resumeBlock = 2;
  save();
  esp_restart();
  return DIAG_IDLE;
}

// ─── Commands ───────────────────────────────────────────────────────────────

static const Result *find(const char *name) {
  for (int i = 0; i < nResults; i++)
    if (!strcmp(results[i].name, name)) return &results[i];
  return nullptr;
}

static void printResults() {
  static const char *const PHASES[] = {"nessuna", "in attesa che stacchi la USB", "in corso", "finita"};
  USBSerial.printf("diagnosi: %s; blocchi da %u min; resistenza batteria %.3f ohm\n", PHASES[phase], blockMinutes, resistance);
  for (int i = 0; i < nResults; i++) {
    const Result &r = results[i];
    if (r.kind == 's')
      USBSerial.printf("  passo %-14s %+7.2f ± %.2f mV  = %7.2f mA  (%u cicli, %u letture fallite)\n", r.name, r.a, r.b, r.c, r.n, r.fails);
    else
      USBSerial.printf("  blocco %-14s %.1f -> %.1f mV in %u min = %.1f mV/h\n", r.name, r.a, r.b, r.n, r.c);
  }
  // The floor: slopes asleep and awake, and the known extra current of being awake.
  const Result *f1 = find("F1-sonno"), *f2 = find("F2-sveglio"), *f3 = find("F3-sonno"), *step = find("sonno");
  const Result *dl = find("L-leggero"), *dd = find("D-profondo");
  if (dl && dd)
    USBSerial.printf("profondo: sonno leggero %.1f mV/h, sonno profondo %.1f mV/h: il chip pesa circa il %.0f%% del consumo a riposo\n", dl->c, dd->c,
                     dl->c > 0 ? 100.0f * (dl->c - dd->c) / dl->c : NAN);
  if (f1 && f2 && f3) {
    float extraMa = step && step->kind == 's' && step->c > 1 ? step->c : 16.2f;
    float asleep = (f1->c + f3->c) / 2, awakeRate = f2->c;
    float perMa = (awakeRate - asleep) / extraMa;  // mV/h for each mA of load
    USBSerial.printf("suolo: a riposo %.1f mV/h (F1 %.1f, F3 %.1f), da sveglio %.1f mV/h; carico noto %.1f mA = %.2f mV/h per mA\n", asleep, f1->c, f3->c, awakeRate, extraMa, perMa);
    if (perMa > 0.05f) USBSerial.printf("  corrente a riposo: %.1f mA (stima; scarto tra i due blocchi a riposo = %.1f mA)\n", asleep / perMa, fabsf(f1->c - f3->c) / perMa);
    else USBSerial.println("  differenza troppo piccola per calcolare: prova ripetuta con blocchi più lunghi");
  }
}

bool diagCommand(const char *cmd) {
  if (!strcmp(cmd, "vbat")) {
    int fails = 0;
    float v = sample(32, 20, fails);
    USBSerial.printf("batteria %.1f mV (%d letture fallite), %d%%, %s, corrente di carica impostata %d mA\n", v, fails, batteryPercent(), chargerState(), chargerCurrentMa());
    return true;
  }
  if (!strncmp(cmd, "i2c ", 4)) {
    // Any device on the shared bus: "i2c 40 06" reads, "i2c 40 06 ff" writes (hex).
    unsigned a = 0, r = 0, v = 0;
    int n = sscanf(cmd + 4, "%x %x %x", &a, &r, &v);
    if (n < 2) return true;
    if (n == 3) {
      Wire.beginTransmission(a);
      Wire.write(r);
      Wire.write(v);
      Wire.endTransmission();
    }
    Wire.beginTransmission(a);
    Wire.write(r);
    int err = Wire.endTransmission(false);
    int got = Wire.requestFrom((uint16_t)a, (uint8_t)1);
    USBSerial.printf("i2c %02X/%02X = %02X (err %d)\n", a, r, got ? Wire.read() : 0xEE, err);
    return true;
  }
  if (!strncmp(cmd, "reg ", 4)) {
    // AXP2101 register: "reg 80" reads, "reg 80 1f" writes (hex).
    unsigned r = 0, v = 0;
    int n = sscanf(cmd + 4, "%x %x", &r, &v);
    if (n == 2) pmuWrite(r, v);
    USBSerial.printf("reg %02X = %02X\n", r, pmuRead(r) & 0xFF);
    return true;
  }
  if (!strncmp(cmd, "calibra", 7)) {
    // USB in and charging at constant current: the charger is the known load.
    int ma = chargerCurrentMa();
    int cycles = atoi(cmd + 7);
    if (cycles <= 0) cycles = 25;
    USBSerial.printf("calibrazione: %s, %d mA, %d cicli...\n", chargerState(), ma, cycles);
    Result r = step("carica", knobCharger, cycles);
    chargerEnable(true);
    // Charger off lowers the voltage: the jump is negative.
    if (ma > 0 && r.a < 0) resistance = -r.a / ma;
    r.c = ma;
    addResult(r);
    USBSerial.printf("salto %.2f ± %.2f mV con %d mA: resistenza %.3f ohm (%s alla fine)\n", r.a, r.b, ma, resistance, chargerState());
    return true;
  }
  if (!strncmp(cmd, "manopola ", 9)) {
    char name[16] = {};
    int on = 1;
    if (sscanf(cmd + 9, "%15s %d", name, &on) < 1) return true;
    for (int i = 0; i < nKnobs; i++)
      if (!strcmp(knobs[i].name, name)) {
        knobs[i].set(on);
        USBSerial.printf("%s %s\n", name, on ? "acceso" : "spento");
        return true;
      }
    USBSerial.println("manopola sconosciuta");
    return true;
  }
  if (!strcmp(cmd, "diagnosi")) {
    printResults();
    return true;
  }
  if (!strcmp(cmd, "diagnosi passi")) {
    floorRun = false;
    stepsOnly = true;
    phase = P_ARMED;
    unpluggedAt = 0;
    save();
    USBSerial.printf("prova dei componenti pronta: stacca la USB e lascia l'orologio fermo per %d minuti\n", 2 + nKnobs + nSleepKnobs + 1);
    return true;
  }
  if (!strncmp(cmd, "diagnosi via", 12)) {
    floorRun = false;
    stepsOnly = false;
    int m = atoi(cmd + 12);
    blockMinutes = m > 0 ? m : 30;
    phase = P_ARMED;
    unpluggedAt = 0;
    save();
    USBSerial.printf("diagnosi pronta: stacca la USB e lascia l'orologio fermo, senza toccarlo, per circa %u minuti\n", 3 * blockMinutes + 8);
    return true;
  }
  if (!strncmp(cmd, "diagnosi suolo", 14)) {
    stepsOnly = true;  // the step series first (calibration and knobs)
    floorRun = true;
    int m = atoi(cmd + 14);
    blockMinutes = m >= 15 ? m : 25;
    phase = P_ARMED;
    unpluggedAt = 0;
    save();
    USBSerial.printf("misura del suolo pronta: stacca la USB e lascia l'orologio fermo, senza toccarlo, per circa %u minuti\n",
                     12 + 3 * blockMinutes + 3);
    return true;
  }
  if (!strncmp(cmd, "diagnosi profondo", 17)) {
    stepsOnly = floorRun = false;
    deepRun = true;
    int m = atoi(cmd + 17);
    blockMinutes = m >= 10 ? m : 40;
    deepV0 = 0;
    deepT0 = 0;
    phase = P_ARMED;
    unpluggedAt = 0;
    save();
    USBSerial.printf("prova del sonno profondo pronta: stacca la USB e lascia l'orologio fermo per circa %u minuti (spento da solo gli ultimi %u)\n",
                     2 + 10 + 25 + blockMinutes, blockMinutes);
    return true;
  }
  if (!strcmp(cmd, "diagnosi stop")) {
    if (phase == P_RUNNING) {
      floorAwake(false);
      finish(true, block == 1 && !floorRun);
    }
    phase = P_NONE;
    floorRun = false;
    deepRun = false;
    save();
    USBSerial.println("diagnosi annullata");
    return true;
  }
  return false;
}

void diagBegin(const Knob *k, int count, void (*stopBle)()) {
  knobs = k;
  nKnobs = count;
  bleStop = stopBle;
  if (!awake) esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "diag", &awake);
  if (!fast) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "diag160", &fast);
  load();
  // A run cut short by a restart cannot resume, except the planned restart
  // after the BLE-off block.
  if (phase == P_RUNNING && resumeBlock < 0) {
    phase = P_DONE;
    save();
  }
}
