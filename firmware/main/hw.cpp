#include "hw.h"
#include <Arduino.h>
#include <Wire.h>
#include "pin_config.h"
#include "SensorPCF85063.hpp"
#include "SensorQMI8658.hpp"
#include "XPowersLib.h"

static SensorPCF85063 rtc;
static SensorQMI8658 imu;
static XPowersPMU pmu;
static bool rtcReady = false, imuReady = false, pmuReady = false;

// Unix seconds at millis() == 0; 0 = unknown.
static int64_t base = 0;
static constexpr int64_t VALID_FROM = 1735689600;  // 2025-01-01: older means the RTC lost power

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

static void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

void hwBegin() {
  // Power manager first: the audio rail (ALDO1) must be up before anything
  // else talks on I2C. Unpowered, the two audio chips hang on the bus lines
  // through the pull-ups and can hold it down (seen after a diagnosis left
  // the rail off: touch and clock failed to start).
  pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuReady) {
    pmu.enableALDO1();
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    // The factory settings turn on every rail, but on this board only DC1
    // (3.3 V system), ALDO1 (audio) and ALDO2 (panel power) are wired to
    // anything (schematic V1.0): the rest only waste current.
    pmu.disableDC2();
    pmu.disableDC3();
    pmu.disableDC4();
    pmu.disableALDO3();
    pmu.disableALDO4();
    pmu.disableBLDO1();
    pmu.disableBLDO2();
    pmu.disableCPUSLDO();
    pmu.disableDLDO1();
    pmu.disableDLDO2();
    // Only the battery's ADC channel runs: the others (USB and system
    // voltage, die temperature) are never read; USB presence is a status bit.
    pmu.disableVbusVoltageMeasure();
    pmu.disableSystemVoltageMeasure();
    pmu.disableTemperatureMeasure();
    // The side key is also the wake key now: a long press cuts the power
    // only when it is really long.
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
    delay(10);
  }
  rtcReady = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (rtcReady) {
    RTC_DateTime t = rtc.getDateTime();
    int64_t unix = daysFromCivil(t.getYear(), t.getMonth(), t.getDay()) * 86400 + t.getHour() * 3600 + t.getMinute() * 60 + t.getSecond();
    if (unix >= VALID_FROM) base = unix - millis() / 1000;
  }
  for (int i = 0; i < 3 && !imuReady; i++) {
    imuReady = imu.begin(Wire, 0x6B, IIC_SDA, IIC_SCL);
    if (!imuReady) delay(100);
  }
  if (imuReady) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
    // No data-ready pulse on INT2 (it would be one per sample, 128 a second,
    // on the pin that wakes the chip): samples are read by polling.
    imu.enableDataReadyINT(false);
  }
}

int64_t clockNow() {
  return base ? base + millis() / 1000 : 0;
}

void clockSet(int64_t unix) {
  if (unix < VALID_FROM) return;
  base = unix - millis() / 1000;
  if (!rtcReady) return;
  int y;
  unsigned m, d;
  civilFromDays(unix / 86400, y, m, d);
  int64_t s = unix % 86400;
  rtc.setDateTime(RTC_DateTime(y, m, d, s / 3600, s / 60 % 60, s % 60));
}

bool motionRead(float &x, float &y, float &z) {
  if (!imuReady || !imu.getDataReady()) return false;
  return imu.getAccelerometer(x, y, z);
}

static bool tapWake = false;
static int tapHz = 250;
static int tapIntPin = 2;
static float tapPeak = 0.3f, tapQuiet = 0.15f;

void motionFast(bool fast) {
  if (!imuReady || tapWake) return;
  imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, fast ? SensorQMI8658::ACC_ODR_125Hz : SensorQMI8658::ACC_ODR_LOWPOWER_21Hz, SensorQMI8658::LPF_MODE_0);
}

// The detector's windows are counted in samples: the datasheet example's
// values at 500 Hz (peak 20, quiet 50, second tap within 250), scaled to the
// rate in use. No low-pass filter: a knock on the glass is a few
// milliseconds long, the filter (a few hertz) erased it entirely (seen).
// Thresholds in g: the example's 0.8 / 0.4 never saw a finger on a watch.
static void tapApply() {
  if (!imuReady) return;
  if (!tapWake) {
    imu.disableTap();
    imu.enableINT(SensorQMI8658::INTERRUPT_PIN_2, false);
    motionFast(false);
    return;
  }
  SensorQMI8658::AccelODR odr = tapHz >= 500 ? SensorQMI8658::ACC_ODR_500Hz : tapHz >= 250 ? SensorQMI8658::ACC_ODR_250Hz : SensorQMI8658::ACC_ODR_LOWPOWER_128Hz;
  int peakWindow = 20 * tapHz / 500, tapWindow = 50 * tapHz / 500, dTapWindow = 250 * tapHz / 500;
  if (peakWindow < 3) peakWindow = 3;
  if (tapWindow < 8) tapWindow = 8;
  if (dTapWindow < 40) dTapWindow = 40;
  imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, odr, SensorQMI8658::LPF_OFF);
  imu.enableAccelerometer();
  imu.configTap(SensorQMI8658::PRIORITY0, (uint8_t)peakWindow, (uint16_t)tapWindow, (uint16_t)dTapWindow, 0.0625f, 0.25f, tapPeak, tapQuiet);
  imu.enableTap(tapIntPin == 1 ? SensorQMI8658::INTERRUPT_PIN_1 : SensorQMI8658::INTERRUPT_PIN_2);
  imu.enableINT(tapIntPin == 1 ? SensorQMI8658::INTERRUPT_PIN_2 : SensorQMI8658::INTERRUPT_PIN_1, false);
  imu.enableDataReadyINT(false);  // the tap events only
  imu.getStatusRegister();  // nothing pending
  imu.getTapStatus();
}

void motionTapWake(bool on) {
  tapWake = on;
  tapApply();
}

bool motionTapWakeOn() {
  return tapWake;
}

void motionTapRate(int hz) {
  tapHz = hz >= 500 ? 500 : hz >= 250 ? 250 : 128;
  if (tapWake) tapApply();
}

int motionTapRateHz() {
  return tapHz;
}

int motionTapEvent(int *status1) {
  if (!imuReady) return 0;
  int s = imu.getStatusRegister();  // STATUS1: reading it clears the flags, and the pin with them
  if (status1) *status1 = s;
  SensorQMI8658::TapEvent e = imu.getTapStatus();  // sticky: the last tap seen, new or not
  if (!(s & SensorQMI8658::EVENT_TAP_MOTION)) return 0;
  return e == SensorQMI8658::DOUBLE_TAP ? 2 : e == SensorQMI8658::SINGLE_TAP ? 1 : 0;
}

void motionTapPin(int pin) {
  tapIntPin = pin == 1 ? 1 : 2;
  if (tapWake) tapApply();
}

int motionTapPinNo() {
  return tapIntPin;
}

void motionTapThreshold(float peak, float quiet) {
  tapPeak = peak;
  tapQuiet = quiet;
  if (tapWake) tapApply();
}

void motionDrdy(bool on) {
  if (imuReady) imu.enableDataReadyINT(on);
}

static int imuReg(uint8_t reg) {
  Wire.beginTransmission(0x6B);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint16_t)0x6B, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

void motionRegs(char *out, size_t size) {
  snprintf(out, size, "CTRL1 %02X CTRL2 %02X CTRL7 %02X CTRL8 %02X STATUSINT %02X STATUS0 %02X STATUS1 %02X TAP %02X", imuReg(0x02), imuReg(0x03), imuReg(0x08),
           imuReg(0x09), imuReg(0x2D), imuReg(0x2E), imuReg(0x2F), imuReg(0x59));
}

void panelPower(bool on) {
  if (!pmuReady) return;
  if (on) pmu.enableALDO2();
  else pmu.disableALDO2();
}

void railsReport(char *out, size_t size) {
  if (!pmuReady) {
    snprintf(out, size, "PMU non disponibile");
    return;
  }
  snprintf(out, size,
           "DC1 %d/%u DC2 %d/%u DC3 %d/%u DC4 %d/%u DC5 %d/%u | ALDO1 %d/%u ALDO2 %d/%u ALDO3 %d/%u ALDO4 %d/%u | BLDO1 %d/%u BLDO2 %d/%u | CPUSLDO %d/%u DLDO1 %d/%u DLDO2 %d/%u",
           pmu.isEnableDC1(), pmu.getDC1Voltage(), pmu.isEnableDC2(), pmu.getDC2Voltage(), pmu.isEnableDC3(), pmu.getDC3Voltage(), pmu.isEnableDC4(),
           pmu.getDC4Voltage(), pmu.isEnableDC5(), pmu.getDC5Voltage(), pmu.isEnableALDO1(), pmu.getALDO1Voltage(), pmu.isEnableALDO2(),
           pmu.getALDO2Voltage(), pmu.isEnableALDO3(), pmu.getALDO3Voltage(), pmu.isEnableALDO4(), pmu.getALDO4Voltage(), pmu.isEnableBLDO1(),
           pmu.getBLDO1Voltage(), pmu.isEnableBLDO2(), pmu.getBLDO2Voltage(), pmu.isEnableCPUSLDO(), pmu.getCPUSLDOVoltage(), pmu.isEnableDLDO1(),
           pmu.getDLDO1Voltage(), pmu.isEnableDLDO2(), pmu.getDLDO2Voltage());
}

int batteryMillivolts() {
  if (!pmuReady || !pmu.isBatteryConnect()) return 0;
  return pmu.getBattVoltage();
}

bool usbPowered() {
  return pmuReady && pmu.isVbusIn();
}

int pmuRead(uint8_t reg) {
  return pmuReady ? pmu.readRegister(reg) : -1;
}

bool pmuWrite(uint8_t reg, uint8_t value) {
  return pmuReady && pmu.writeRegister(reg, value) == 0;
}

void chargerEnable(bool on) {
  if (!pmuReady) return;
  if (on) pmu.enableCellbatteryCharge();
  else pmu.disableCellbatteryCharge();
}

int chargerCurrentMa() {
  int v = pmuRead(0x62);  // ICC_CHG_SET: 25 mA steps up to 200, then 100 mA steps
  if (v < 0) return -1;
  v &= 0x1F;
  return v <= 8 ? 25 * v : 200 + 100 * (v - 8);
}

int chargerPhase() {
  if (!pmuReady || !pmu.isVbusIn()) return 0;
  switch (pmu.getChargerStatus()) {
    case XPOWERS_AXP2101_CHG_PRE_STATE: return 1;
    case XPOWERS_AXP2101_CHG_CC_STATE: return 2;
    case XPOWERS_AXP2101_CHG_CV_STATE: return 3;
    case XPOWERS_AXP2101_CHG_DONE_STATE: return 4;
    default: return 5;
  }
}

const char *chargerState() {
  if (!pmuReady) return "?";
  if (!pmu.isVbusIn()) return "scarica";
  switch (pmu.getChargerStatus()) {
    case XPOWERS_AXP2101_CHG_TRI_STATE: return "mantenimento";
    case XPOWERS_AXP2101_CHG_PRE_STATE: return "precarica";
    case XPOWERS_AXP2101_CHG_CC_STATE: return "carica a corrente costante";
    case XPOWERS_AXP2101_CHG_CV_STATE: return "carica a tensione costante";
    case XPOWERS_AXP2101_CHG_DONE_STATE: return "carica completa";
    case XPOWERS_AXP2101_CHG_STOP_STATE: return "carica ferma";
    default: return "?";
  }
}

void audioRail(bool on) {
  if (!pmuReady) return;
  if (on) pmu.enableALDO1();
  else pmu.disableALDO1();
}

void motionPower(bool on) {
  if (!imuReady) return;
  if (on) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_LOWPOWER_21Hz, SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
  } else {
    imu.disableAccelerometer();
  }
}

int batteryPercent() {
  if (!pmuReady || !pmu.isBatteryConnect()) return -1;
  return pmu.getBatteryPercent();
}
