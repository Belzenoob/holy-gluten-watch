#include "audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include "driver/gpio.h"

// Board wiring (Waveshare BSP esp32_c6_touch_amoled_2_06).
static constexpr int8_t PIN_MCLK = 19, PIN_BCLK = 20, PIN_WS = 22, PIN_DOUT = 23, PIN_DIN = 21;
static constexpr uint8_t PIN_AMP = 6;  // speaker amplifier enable
static constexpr uint8_t ES8311 = 0x18;
static constexpr uint8_t ES7210 = 0x40;  // the microphones' ADC, unused
static constexpr int8_t I2S_PINS[] = {PIN_MCLK, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN};
// DAC volume, 0xBF = 0 dB, 0.5 dB per step: a little below full, it is a wrist.
static constexpr uint8_t VOLUME = 0xB4;

static I2SClass i2s;
static QueueHandle_t requests = nullptr;
static bool codecReady = false;
static bool muted = false;

static bool regAt(uint8_t chip, uint8_t r, uint8_t v) {
  Wire.beginTransmission(chip);
  Wire.write(r);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool reg(uint8_t r, uint8_t v) {
  Wire.beginTransmission(ES8311);
  Wire.write(r);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool i2sStart() {
  i2s.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN, PIN_MCLK);
  return i2s.begin(I2S_MODE_STD, SOUND_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
}

// The ES8311 as a slave DAC, MCLK = 256 fs from the ESP32 (sequence of
// Espressif's esp_codec_dev es8311 driver: open, sample rate, start).
static bool codecStart() {
  bool ok = true;
  // Open.
  ok &= reg(0x45, 0x00);
  ok &= reg(0x01, 0x30);
  ok &= reg(0x02, 0x00);
  ok &= reg(0x03, 0x10);
  ok &= reg(0x16, 0x24);
  ok &= reg(0x04, 0x10);
  ok &= reg(0x05, 0x00);
  ok &= reg(0x0B, 0x00);
  ok &= reg(0x0C, 0x00);
  ok &= reg(0x10, 0x1F);
  ok &= reg(0x11, 0x7F);
  ok &= reg(0x00, 0x80);  // power on, slave
  ok &= reg(0x01, 0x3F);  // all clocks on, MCLK from its pin
  ok &= reg(0x13, 0x10);
  ok &= reg(0x1B, 0x0A);
  ok &= reg(0x1C, 0x6A);
  // 16 kHz with MCLK 4.096 MHz: no dividers, single speed, OSR 0x10.
  ok &= reg(0x02, 0x00);
  ok &= reg(0x05, 0x00);
  ok &= reg(0x03, 0x10);
  ok &= reg(0x04, 0x10);
  ok &= reg(0x07, 0x00);
  ok &= reg(0x08, 0xFF);
  ok &= reg(0x06, 0x03);
  // I2S, 16 bit; DAC input on, ADC output muted (no recording).
  ok &= reg(0x09, 0x0C);
  ok &= reg(0x0A, 0x4C);
  // Start the DAC path.
  ok &= reg(0x17, 0xBF);
  ok &= reg(0x0E, 0x02);
  ok &= reg(0x12, 0x00);
  ok &= reg(0x14, 0x1A);
  ok &= reg(0x0D, 0x01);
  ok &= reg(0x15, 0x40);
  ok &= reg(0x37, 0x08);
  ok &= reg(0x45, 0x00);
  ok &= reg(0x31, 0x00);  // unmute
  ok &= reg(0x32, VOLUME);
  return ok;
}

// Between sounds the codec goes to standby (Espressif's es8311 suspend
// sequence) and the amplifier pin is held low, also through light sleep, where
// an unheld pin could float and switch the amplifier back on.
static void codecSuspend() {
  reg(0x32, 0x00);
  reg(0x17, 0x00);
  reg(0x0E, 0xFF);
  reg(0x12, 0x02);
  reg(0x14, 0x00);
  reg(0x0D, 0xFA);
  reg(0x15, 0x00);
  reg(0x02, 0x10);
  reg(0x00, 0x00);
  reg(0x00, 0x1F);
  reg(0x01, 0x30);
  reg(0x01, 0x00);
  reg(0x45, 0x00);
  reg(0x0D, 0xFC);
  reg(0x02, 0x00);
}

static void amp(bool on) {
  gpio_hold_dis((gpio_num_t)PIN_AMP);
  digitalWrite(PIN_AMP, on ? HIGH : LOW);
  if (!on) gpio_hold_en((gpio_num_t)PIN_AMP);
}

static void audioTask(void *) {
  static int16_t silence[256] = {0};
  for (;;) {
    uint8_t s;
    if (xQueueReceive(requests, &s, portMAX_DELAY) != pdTRUE || s >= SND_COUNT) continue;
    if (!i2sStart()) continue;
    codecStart();  // clocks are back: make sure the DAC path is up
    amp(true);
    bool more = true;
    while (more) {
      const SoundClip &clip = SOUNDS[s];
      for (uint32_t at = 0; at < clip.count;) {
        if (uxQueueMessagesWaiting(requests)) break;  // a newer sound wins
        uint32_t n = clip.count - at < 512 ? clip.count - at : 512;
        i2s.write((const uint8_t *)(clip.samples + at), n * sizeof(int16_t));
        at += n;
      }
      more = xQueueReceive(requests, &s, 0) == pdTRUE && s < SND_COUNT;
    }
    // Let the last samples out of the DMA buffers, then go quiet.
    for (int i = 0; i < 4; i++) i2s.write((const uint8_t *)silence, sizeof(silence));
    vTaskDelay(pdMS_TO_TICKS(30));
    amp(false);
    codecSuspend();
    i2s.end();
    audioPinsPulled(true);
  }
}

// The ES7210 shares the audio rail and the I2S clocks but nothing here
// records: from power-on it sits with its analog part up, so it goes to
// standby (Espressif's es7210 stop sequence). On = back to power-on values.
void micPower(bool on) {
  for (uint8_t r = 0x47; r <= 0x4A; r++) regAt(ES7210, r, on ? 0x00 : 0xFF);  // MIC1..4 power
  regAt(ES7210, 0x4B, 0xFF);                                                  // MIC12 / MIC34 off
  regAt(ES7210, 0x4C, 0xFF);
  regAt(ES7210, 0x40, on ? 0x80 : 0xC0);  // analog: references off
  regAt(ES7210, 0x01, on ? 0x20 : 0x7F);  // clocks off
  regAt(ES7210, 0x06, on ? 0x00 : 0x07);  // power down
}

// Between sounds the I2S lines are inputs pulled down, also through light
// sleep (which would otherwise let them float): left floating, the two
// codecs' clock inputs cost about 1 mA (measured). `pulled` false = floating,
// for the diagnosis.
void audioPinsPulled(bool pulled) {
  for (int8_t pin : I2S_PINS) {
    pinMode(pin, pulled ? INPUT_PULLDOWN : INPUT);
    if (pulled) gpio_sleep_sel_dis((gpio_num_t)pin);
    else gpio_sleep_sel_en((gpio_num_t)pin);
  }
}

void audioBegin() {
  Preferences p;
  if (p.begin("madre", true)) {
    muted = p.getBool("mute", false);
    p.end();
  }
  pinMode(PIN_AMP, OUTPUT);
  amp(false);
  micPower(false);
  // Check the codec once at boot, with its clocks running, then let it rest.
  if (i2sStart()) {
    codecReady = codecStart();
    codecSuspend();
    i2s.end();
    audioPinsPulled(true);
  }
  if (!codecReady) return;
  requests = xQueueCreate(2, sizeof(uint8_t));
  xTaskCreate(audioTask, "audio", 4096, nullptr, 3, nullptr);
}

bool audioReady() {
  return codecReady;
}

bool audioMuted() {
  return muted;
}

void audioSetMuted(bool m) {
  muted = m;
  Preferences p;
  if (p.begin("madre", false)) {
    p.putBool("mute", m);
    p.end();
  }
}

void audioPlay(Sound s) {
  if (!requests || muted) return;
  uint8_t v = s;
  xQueueSend(requests, &v, 0);
}
