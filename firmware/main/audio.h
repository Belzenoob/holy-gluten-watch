#pragma once
#include "sounds.h"

// The pet's voice: ES8311 codec over I2S, playing the clips in sounds.cpp on
// a task of its own so the animation never waits. The speaker amplifier and
// the I2S clocks are on only while a sound plays.

void audioBegin();
bool audioReady();
// Mute switch (a swipe up on the watch); kept across restarts.
bool audioMuted();
void audioSetMuted(bool muted);
// Plays a sound; a newer one cuts the current one short.
void audioPlay(Sound s);
// The microphones' ADC (ES7210): standby from boot (no recording here); on = power-on state.
void micPower(bool on);
// I2S lines between sounds: pulled down, also in light sleep (the default
// from boot); false = floating, for the diagnosis.
void audioPinsPulled(bool pulled);
