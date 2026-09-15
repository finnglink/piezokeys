// PiezoKeys -- 4-key chorded USB MIDI keyboard, Seeed XIAO RP2040
//
// Hardware: 4 piezos on A0-A3 (key1..key4), 4x SK6812 RGBW LEDs on D4,
// wired in reverse of the keys (LED 0 sits at key 4's end) -- see
// KEY_TO_LED below.
//
// Chord model: any combination of the 4 keys struck within a short window
// is one "combo" (15 possible non-empty combos of 4 keys), mapped to one
// of 15 scale degrees spanning 2 octaves -- see DEGREE_FOR_COMBO.
//
// Hold behaviour: measured piezo data shows a strike's initial peak (e.g.
// ~2290) decays to a lingering plateau of only ~76 within roughly half a
// second of a static hold -- a piezo mainly reports *change* in strain,
// which bleeds off through its bias resistor even under continued
// pressure. A held note is tracked with a real release state: once a
// chord triggers a Note On, its member channels are watched against a
// separate, much lower RELEASE_THRESHOLD (below the measured hold
// plateau but above baseline noise); once all of them dip below it for
// RELEASE_DEBOUNCE_MS, the note is actually released. This is a binary
// held/released check only -- not continuous aftertouch.
//
// Key 4 (A3) hardware note: measured data shows a persistent low-level
// hum (~55-73 counts, baseline ~26) present continuously regardless of
// activity -- not real taps. Likely a loose connection or bias-resistor
// issue. Thresholds are shared across all 4 channels for now at the
// user's request; TRIGGER_THRESHOLD_KEY4_FALLBACK below records the
// higher value (~150) that was needed to clear key 4's hum, in case it
// needs to be reinstated before the hardware is fixed.
//
// Menu: tap all 4 keys together twice (each within GESTURE_REPEAT_WINDOW_MS
// of the previous) to enter/exit. Layer 1 is 4 single-key categories
// (Root/Scale/Transpose/unassigned). Layer 2 reuses the same 15-combo
// vocabulary as play mode to pick a value within that category.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <math.h>

Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

// ---------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------

const int PIEZO_PINS[4] = {A0, A1, A2, A3};

const int PIXEL_PIN = D4;
const int NUM_PIXELS = 4;
Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRBW + NEO_KHZ800);

// LEDs are wired in reverse of the keys (pixel 0 sits at key 4's end).
const uint8_t KEY_TO_LED[4] = {3, 2, 1, 0};

// ---------------------------------------------------------------------
// Piezo sampling / chord detection tuning
// ---------------------------------------------------------------------

const uint32_t SAMPLE_INTERVAL_US = 200; // 5000 samples/sec per channel

// Shared trigger threshold across all 4 channels for now (see header note
// re: key 4's hum). TRIGGER_THRESHOLD_KEY4_FALLBACK is the previously
// measured value that safely cleared key 4's hum, kept here for reference.
// Threshold reduced ~5% from the original 90 to give softer taps and
// multi-key presses more headroom -- watch whether this is still enough
// to reliably reject noise, especially on key 4.
const int TRIGGER_THRESHOLD_DEFAULT = 85; // keys 2 and 3
// Keys 1 and 4 sit on the outer, stiffer sides of the shell and read
// consistently weaker for the same physical force. Split into separate
// per-key constants now that they've diverged: key 1 has no known noise
// problem, so it's been pushed lower still for responsiveness (its
// original-capture incidental-bump ceiling was ~48, well below this).
// Key 4 is kept higher since it has less room -- it sits close to its
// previously measured hardware hum ceiling (~73, see header note);
// TRIGGER_THRESHOLD_KEY4_FALLBACK is the old, hum-safe value to fall back
// to if false triggers resurface there.
const int TRIGGER_THRESHOLD_KEY1 = 62;
const int TRIGGER_THRESHOLD_KEY4 = 78;
const int TRIGGER_THRESHOLD_KEY4_FALLBACK = 150; // unused for now, see header note
const int TRIGGER_THRESHOLD[4] = {
  TRIGGER_THRESHOLD_KEY1, TRIGGER_THRESHOLD_DEFAULT,
  TRIGGER_THRESHOLD_DEFAULT, TRIGGER_THRESHOLD_KEY4,
};

// Retrigger detection during a hold needs a bar clearly above both (a) a
// held note's own natural pressure jitter -- real data shows a firm
// (not just resting) hold can produce transients well past the old
// 150 -- and (b) key 1's slower-rising attack, whose peak sometimes isn't
// fully captured by the initial chord window and keeps climbing past it,
// which the old lower bar misread as a brand new strike (visible as a
// low-velocity note immediately followed by a higher-velocity one).
// Raised well above ordinary medium-tap territory so only a clearly
// deliberate new hit counts -- trade-off: very soft, fast re-strikes on
// the same key while still held may not register until release instead.
const int RETRIGGER_THRESHOLD = 500;

// Retrigger checks are ignored for this long right after note-on, giving
// a slow-rising attack (key 1 especially) time to finish climbing instead
// of having its own continued rise misread as a second strike.
const uint32_t RETRIGGER_GRACE_MS = 80;

// A channel only counts toward a chord if its peak is also at least this
// fraction of the loudest channel's peak in the same window -- rejects
// crosstalk from a hard hit on a neighbouring key (worst case measured:
// ~37% crosstalk). Lowered from 0.5 to give real multi-key presses more
// slack -- the piezos aren't equally sensitive (key 1 in particular reads
// noticeably weaker than the others for the same physical force) and a
// soft double-tap's natural per-finger variance is a much larger fraction
// of its peak than a hard hit's, so the old, stricter ratio was rejecting
// genuine multi-key hits more often at low velocity or on key 1. This
// value sits just below the worst crosstalk case we measured, so very
// hard hits may occasionally register a neighbour's crosstalk as part of
// the chord -- tune back up if that becomes a real problem.
const float DOMINANCE_RATIO = 0.4f;

// Widened (was 20ms, briefly) -- multi-key detection reliability matters
// more than shaving a few ms of latency; gives fingers more room to land
// within the same window.
const uint32_t PLAY_CHORD_WINDOW_MS = 45;
const uint32_t MENU_CHORD_WINDOW_MS = 200;

const int RELEASE_THRESHOLD = 45;          // below this (all member channels) counts as released
const uint32_t RELEASE_DEBOUNCE_MS = 180;  // must stay below release threshold this long to confirm release
                                            // (widened so a brief pressure dip mid-hold doesn't end the note)
const uint32_t MIN_NOTE_DURATION_MS = 50;  // minimum audible duration even on an instant tap-release
const uint32_t MAX_HOLD_MS = 4000;         // safety cap in case release is never cleanly detected

const uint32_t REFRACTORY_MS = 150;    // per-key debounce after release, starts after note-off
const uint32_t MENU_COOLDOWN_MS = 200; // debounce after a menu selection
const uint32_t MENU_IDLE_TIMEOUT_MS = 6000; // auto-exit the menu after this long with no input

const int VELOCITY_ADC_FLOOR = 85;
const int VELOCITY_ADC_CEILING = 4095; // piezos saturate the ADC on a hard hit
const float VELOCITY_GAMMA = 0.5f;     // <1 boosts low-end response
const int MIN_VELOCITY = 10;

// Widened from 700ms -- the held first all-4 tap has to actually release
// (RELEASE_DEBOUNCE_MS + per-key REFRACTORY_MS, plus however long it takes
// to lift all 4 fingers) before the engine can even see the second tap,
// which was eating most of the old window.
const uint32_t GESTURE_REPEAT_WINDOW_MS = 1500; // max gap between the 2 all-4 taps

const float LED_MIN_BRIGHT_FRAC = 0.15f;

// ---------------------------------------------------------------------
// Note / scale / menu tables
// ---------------------------------------------------------------------

const uint8_t ROOT_BASE_NOTE = 60; // MIDI note for root's "C" at transpose 0
const uint8_t MIDI_CHANNEL = 1;

struct RGBW { uint8_t r, g, b, w; };

const RGBW COLOR_WHITE     = {0, 0, 0, 255};
const RGBW COLOR_BLUE      = {0, 0, 255, 0};
const RGBW COLOR_TURQUOISE = {0, 206, 180, 0};
const RGBW COLOR_GREEN     = {0, 255, 0, 0};
const RGBW COLOR_PURPLE    = {140, 0, 255, 0};
const RGBW COLOR_ORANGE    = {255, 90, 0, 0};
const RGBW COLOR_RED       = {255, 0, 0, 0};

const RGBW CAT_COLOR_ROOT       = {255, 0, 0, 0};
const RGBW CAT_COLOR_SCALE      = {0, 0, 255, 0};
const RGBW CAT_COLOR_TRANSPOSE  = {0, 255, 0, 0};
const RGBW CAT_COLOR_UNASSIGNED = {0, 0, 0, 40};

struct ScaleDef { const char *name; int8_t intervals[7]; };

const ScaleDef SCALE_TABLE[6] = {
  {"Major",         {0, 2, 4, 5, 7, 9, 11}},
  {"NaturalMinor",  {0, 2, 3, 5, 7, 8, 10}},
  {"Dorian",        {0, 2, 3, 5, 7, 9, 10}},
  {"Mixolydian",    {0, 2, 4, 5, 7, 9, 10}},
  {"HarmonicMinor", {0, 2, 3, 5, 7, 8, 11}},
  {"MelodicMinor",  {0, 2, 3, 5, 7, 9, 11}},
};

// combo bitmask (bit0=key1 .. bit3=key4) -> scale degree 1-15. Index 0 unused.
const uint8_t DEGREE_FOR_COMBO[16] = {
  0,  1,  3,  2,  5,  9,  4, 11,
  7,  8, 10, 12,  6, 13, 14, 15,
};

// combo bitmask -> play-mode LED color (encodes chord *shape*)
const RGBW COLOR_FOR_COMBO[16] = {
  COLOR_WHITE, // 0 unused
  COLOR_WHITE, COLOR_WHITE, COLOR_BLUE, COLOR_WHITE, COLOR_TURQUOISE, COLOR_BLUE, COLOR_PURPLE,
  COLOR_WHITE, COLOR_GREEN, COLOR_TURQUOISE, COLOR_ORANGE, COLOR_BLUE, COLOR_ORANGE, COLOR_PURPLE, COLOR_RED,
};

// combo bitmask -> chromatic root offset 0-11 (C..B), -1 = not assigned
const int8_t ROOT_FOR_COMBO[16] = {
  -1,  0,  2,  1,  4,  8,  3, 10,
   6,  7,  9, 11,  5, -1, -1, -1,
};

// combo bitmask -> index into SCALE_TABLE, -1 = not assigned
const int8_t SCALE_FOR_COMBO[16] = {
  -1,  0,  2,  1,  4, -1,  3, -1,
  -1, -1, -1, -1,  5, -1, -1, -1,
};

// combo bitmask -> transpose in semitones, INVALID_TRANSPOSE = not assigned
const int INVALID_TRANSPOSE = -1000;
const int TRANSPOSE_FOR_COMBO[16] = {
  INVALID_TRANSPOSE, -12,  -6, -24,
                   6, -18,   0, INVALID_TRANSPOSE,
                  12, INVALID_TRANSPOSE, 18, INVALID_TRANSPOSE,
                  24, INVALID_TRANSPOSE, INVALID_TRANSPOSE, INVALID_TRANSPOSE,
};

// ---------------------------------------------------------------------
// Settings persistence (RP2040 emulated EEPROM, survives reboot/power-off)
// ---------------------------------------------------------------------

const int EEPROM_SIZE = 16;
const uint8_t SETTINGS_MAGIC = 0xA5; // marks that a valid settings block was saved

struct __attribute__((packed)) PersistedSettings {
  uint8_t magic;
  int8_t rootOffset;
  int8_t scaleIndex;
  int16_t transposeSemitones;
};

// ---------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------

enum Mode { MODE_PLAY, MODE_MENU_TOP, MODE_MENU_ROOT, MODE_MENU_SCALE, MODE_MENU_TRANSPOSE };
Mode mode = MODE_PLAY;

int currentRootOffset = 0;       // 0-11
int currentScaleIndex = 0;       // index into SCALE_TABLE
int currentTransposeSemitones = 0;

enum EngineState { ENGINE_IDLE, ENGINE_CAPTURING, ENGINE_NOTE_HELD, ENGINE_MENU_COOLDOWN };
EngineState engineState = ENGINE_IDLE;

uint32_t nextSampleAt = 0;
uint32_t windowEndAt = 0;
int windowPeak[4] = {0, 0, 0, 0};

uint8_t heldCombo = 0;
uint8_t heldNoteNumber = 0;
uint32_t heldNoteOnAt = 0;
uint32_t releaseCandidateSinceMs = 0; // 0 = not currently a release candidate

uint32_t menuCooldownEndAt = 0;

uint32_t keyRefractoryUntil[4] = {0, 0, 0, 0};
bool armedForTrigger[4] = {true, true, true, true}; // must dip back below threshold before re-triggering

uint8_t allFourTapCount = 0;
uint32_t lastAllFourTapAt = 0;

uint32_t lastMenuActivityAt = 0;

// ---------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------

uint32_t packRGBW(RGBW c) { return pixels.Color(c.r, c.g, c.b, c.w); }

void clearAllLEDs() {
  for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, 0);
  pixels.show();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  PersistedSettings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    currentRootOffset = s.rootOffset;
    currentScaleIndex = s.scaleIndex;
    currentTransposeSemitones = s.transposeSemitones;
    Serial.println("Settings loaded from flash");
  }
}

void saveSettings() {
  PersistedSettings s;
  s.magic = SETTINGS_MAGIC;
  s.rootOffset = (int8_t)currentRootOffset;
  s.scaleIndex = (int8_t)currentScaleIndex;
  s.transposeSemitones = (int16_t)currentTransposeSemitones;
  EEPROM.put(0, s);
  EEPROM.commit();
}

// Blink the given combo's LED pattern on/off `times` times, in `color` --
// used to confirm a menu selection actually took.
void blinkCombo(RGBW color, uint8_t combo, int times) {
  for (int rep = 0; rep < times; rep++) {
    clearAllLEDs();
    delay(120);
    for (int i = 0; i < 4; i++) {
      if (combo & (1 << i)) pixels.setPixelColor(KEY_TO_LED[i], packRGBW(color));
    }
    pixels.show();
    delay(180);
  }
  clearAllLEDs();
  delay(100);
}

void startMenuCooldown(uint32_t nowMs) {
  menuCooldownEndAt = nowMs + MENU_COOLDOWN_MS;
  engineState = ENGINE_MENU_COOLDOWN;
}

void releaseHeldNote(uint32_t nowMs) {
  MIDI.sendNoteOff(heldNoteNumber, 0, MIDI_CHANNEL);
  for (int i = 0; i < 4; i++) {
    if (heldCombo & (1 << i)) {
      pixels.setPixelColor(KEY_TO_LED[i], 0);
      keyRefractoryUntil[i] = nowMs + REFRACTORY_MS;
    }
  }
  pixels.show();
  Serial.print("NOTE OFF note="); Serial.println(heldNoteNumber);
  engineState = ENGINE_IDLE;
  releaseCandidateSinceMs = 0;
}

float velocityFraction(int peak) {
  float frac = (float)(peak - VELOCITY_ADC_FLOOR) / (float)(VELOCITY_ADC_CEILING - VELOCITY_ADC_FLOOR);
  frac = constrain(frac, 0.0f, 1.0f);
  return powf(frac, VELOCITY_GAMMA);
}

uint8_t peakToVelocity(int peak) {
  float shaped = velocityFraction(peak);
  int vel = MIN_VELOCITY + (int)roundf(shaped * (127 - MIN_VELOCITY));
  return (uint8_t)constrain(vel, 1, 127);
}

uint32_t scaleColor(RGBW c, float frac) {
  float b = LED_MIN_BRIGHT_FRAC + frac * (1.0f - LED_MIN_BRIGHT_FRAC);
  return pixels.Color((uint8_t)(c.r * b), (uint8_t)(c.g * b), (uint8_t)(c.b * b), (uint8_t)(c.w * b));
}

int degreeToSemitoneOffset(int degree) {
  int octave = (degree - 1) / 7;
  int idx = (degree - 1) % 7;
  return 12 * octave + SCALE_TABLE[currentScaleIndex].intervals[idx];
}

uint8_t comboForRootOffset(int offset) {
  for (int c = 1; c <= 15; c++) if (ROOT_FOR_COMBO[c] == offset) return (uint8_t)c;
  return 0;
}

uint8_t comboForScaleIndex(int idx) {
  for (int c = 1; c <= 15; c++) if (SCALE_FOR_COMBO[c] == idx) return (uint8_t)c;
  return 0;
}

uint8_t comboForTranspose(int semis) {
  for (int c = 1; c <= 15; c++) if (TRANSPOSE_FOR_COMBO[c] == semis) return (uint8_t)c;
  return 0;
}

void hsvToRgb(float h, uint8_t &r, uint8_t &g, uint8_t &b) {
  float c = 1.0f;
  float x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
  float rf, gf, bf;
  if (h < 60)       { rf = c; gf = x; bf = 0; }
  else if (h < 120) { rf = x; gf = c; bf = 0; }
  else if (h < 180) { rf = 0; gf = c; bf = x; }
  else if (h < 240) { rf = 0; gf = x; bf = c; }
  else if (h < 300) { rf = x; gf = 0; bf = c; }
  else              { rf = c; gf = 0; bf = x; }
  r = (uint8_t)(rf * 255);
  g = (uint8_t)(gf * 255);
  b = (uint8_t)(bf * 255);
}

// ---------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------

void showMenuTopLEDs() {
  pixels.setPixelColor(KEY_TO_LED[0], packRGBW(CAT_COLOR_ROOT));
  pixels.setPixelColor(KEY_TO_LED[1], packRGBW(CAT_COLOR_SCALE));
  pixels.setPixelColor(KEY_TO_LED[2], packRGBW(CAT_COLOR_TRANSPOSE));
  pixels.setPixelColor(KEY_TO_LED[3], packRGBW(CAT_COLOR_UNASSIGNED));
  pixels.show();
}

// Brief blocking flash of the category color (all 4), then settles to
// showing the current value as the lit-LED pattern of its combo -- the
// same pattern you'd press to select it.
void showMenuPage(RGBW color, uint8_t currentCombo) {
  for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, packRGBW(color));
  pixels.show();
  delay(150);
  clearAllLEDs();
  for (int i = 0; i < 4; i++) {
    if (currentCombo & (1 << i)) pixels.setPixelColor(KEY_TO_LED[i], packRGBW(color));
  }
  pixels.show();
}

void toggleMenu() {
  if (mode == MODE_PLAY) {
    mode = MODE_MENU_TOP;
    lastMenuActivityAt = millis();
    showMenuTopLEDs();
    Serial.println("MENU enter");
  } else {
    mode = MODE_PLAY;
    clearAllLEDs();
    Serial.println("MENU exit");
  }
}

void handleMenuCombo(uint8_t combo, uint32_t nowMs) {
  lastMenuActivityAt = nowMs;

  if (mode == MODE_MENU_TOP) {
    if (combo == 0b0001) {
      mode = MODE_MENU_ROOT;
      showMenuPage(CAT_COLOR_ROOT, comboForRootOffset(currentRootOffset));
    } else if (combo == 0b0010) {
      mode = MODE_MENU_SCALE;
      showMenuPage(CAT_COLOR_SCALE, comboForScaleIndex(currentScaleIndex));
    } else if (combo == 0b0100) {
      mode = MODE_MENU_TRANSPOSE;
      showMenuPage(CAT_COLOR_TRANSPOSE, comboForTranspose(currentTransposeSemitones));
    }
    // combo 0b1000 (key4) and any multi-key combo at top level: no-op.
    startMenuCooldown(nowMs);
    return;
  }

  bool handled = false;
  RGBW confirmColor = COLOR_WHITE;
  if (mode == MODE_MENU_ROOT && ROOT_FOR_COMBO[combo] != -1) {
    currentRootOffset = ROOT_FOR_COMBO[combo];
    handled = true;
    confirmColor = CAT_COLOR_ROOT;
    Serial.print("MENU root -> "); Serial.println(currentRootOffset);
  } else if (mode == MODE_MENU_SCALE && SCALE_FOR_COMBO[combo] != -1) {
    currentScaleIndex = SCALE_FOR_COMBO[combo];
    handled = true;
    confirmColor = CAT_COLOR_SCALE;
    Serial.print("MENU scale -> "); Serial.println(SCALE_TABLE[currentScaleIndex].name);
  } else if (mode == MODE_MENU_TRANSPOSE && TRANSPOSE_FOR_COMBO[combo] != INVALID_TRANSPOSE) {
    currentTransposeSemitones = TRANSPOSE_FOR_COMBO[combo];
    handled = true;
    confirmColor = CAT_COLOR_TRANSPOSE;
    Serial.print("MENU transpose -> "); Serial.println(currentTransposeSemitones);
  }

  if (handled) {
    blinkCombo(confirmColor, combo, 2); // confirm: blink the new value's pattern twice
    saveSettings();
    mode = MODE_MENU_TOP;
    showMenuTopLEDs();
  }
  startMenuCooldown(nowMs);
}

// ---------------------------------------------------------------------
// Play mode
// ---------------------------------------------------------------------

void playCombo(uint8_t combo, uint32_t nowMs) {
  int maxPeak = 0;
  for (int i = 0; i < 4; i++) {
    if ((combo & (1 << i)) && windowPeak[i] > maxPeak) maxPeak = windowPeak[i];
  }

  int degree = DEGREE_FOR_COMBO[combo];
  int semitoneOffset = degreeToSemitoneOffset(degree);
  int note = ROOT_BASE_NOTE + currentRootOffset + currentTransposeSemitones + semitoneOffset;
  note = constrain(note, 0, 127);

  uint8_t velocity = peakToVelocity(maxPeak);
  MIDI.sendNoteOn(note, velocity, MIDI_CHANNEL);

  float brightFrac = velocityFraction(maxPeak);
  RGBW baseColor = COLOR_FOR_COMBO[combo];
  for (int i = 0; i < 4; i++) {
    if (combo & (1 << i)) pixels.setPixelColor(KEY_TO_LED[i], scaleColor(baseColor, brightFrac));
  }
  pixels.show();

  Serial.print("NOTE ON combo=0b");
  Serial.print(combo, BIN);
  Serial.print(" degree="); Serial.print(degree);
  Serial.print(" note="); Serial.print(note);
  Serial.print(" vel="); Serial.println(velocity);

  for (int i = 0; i < 4; i++) if (combo & (1 << i)) armedForTrigger[i] = false;

  heldCombo = combo;
  heldNoteNumber = (uint8_t)note;
  heldNoteOnAt = nowMs;
  releaseCandidateSinceMs = 0;
  engineState = ENGINE_NOTE_HELD;
}

// ---------------------------------------------------------------------
// Chord engine
// ---------------------------------------------------------------------

void finalizeChord(uint32_t nowMs) {
  int maxPeak = 0;
  for (int i = 0; i < 4; i++) if (windowPeak[i] > maxPeak) maxPeak = windowPeak[i];

  uint8_t combo = 0;
  for (int i = 0; i < 4; i++) {
    if (windowPeak[i] >= TRIGGER_THRESHOLD[i] && windowPeak[i] >= DOMINANCE_RATIO * maxPeak) {
      combo |= (1 << i);
    }
  }

  if (combo == 0) { engineState = ENGINE_IDLE; return; }

  if (combo == 0b1111) {
    if (nowMs - lastAllFourTapAt <= GESTURE_REPEAT_WINDOW_MS) allFourTapCount++;
    else allFourTapCount = 1;
    lastAllFourTapAt = nowMs;
    if (allFourTapCount >= 2) {
      allFourTapCount = 0;
      toggleMenu();
      startMenuCooldown(nowMs);
      return;
    }
  } else {
    allFourTapCount = 0;
  }

  if (mode == MODE_PLAY) playCombo(combo, nowMs);
  else handleMenuCombo(combo, nowMs);
}

// ---------------------------------------------------------------------
// Boot animation
// ---------------------------------------------------------------------

const uint32_t BOOT_HUE_SWEEP_MS = 800;
const uint32_t BOOT_HUE_EDGE_MS = BOOT_HUE_SWEEP_MS / 6; // 60 of 360 degrees
const float BOOT_HUE_MAX_DEG = 300.0f; // hue advances up to here, then holds -- never wraps back to red
const uint32_t BOOT_STAGGER_MS = BOOT_HUE_SWEEP_MS / 2;
const uint32_t BOOT_PHASE1_TOTAL_MS = 3 * BOOT_STAGGER_MS + BOOT_HUE_SWEEP_MS;
const uint32_t BOOT_POST_PHASE1_DELAY_MS = 150;
const uint32_t BOOT_W_FADE_MS = 350;
const uint32_t BOOT_W_STAGGER_MS = 100;
const uint32_t BOOT_HOLD_MS = 350;
const uint32_t BOOT_FADE_OUT_MS = 400;
const uint32_t BOOT_TICK_MS = 20;

void runBootAnimation() {
  uint32_t phaseStart = millis();
  uint32_t elapsed;
  do {
    elapsed = millis() - phaseStart;
    for (int i = 0; i < 4; i++) {
      uint32_t ledStart = (uint32_t)i * BOOT_STAGGER_MS;
      if (elapsed < ledStart) continue; // not started yet, leave off
      if (elapsed >= ledStart + BOOT_HUE_SWEEP_MS) {
        pixels.setPixelColor(KEY_TO_LED[i], 0); // sweep finished -- force fully off, don't
        continue;                               // just leave it frozen at its last faded value
      }
      uint32_t t = elapsed - ledStart;
      // Hue advances only across the fade-in + full-brightness middle; it
      // holds at BOOT_HUE_MAX_DEG through the fade-out so brightness ramps
      // to zero on whatever color it stopped on, instead of cycling back
      // around to red at the very end.
      uint32_t hueActiveMs = BOOT_HUE_SWEEP_MS - BOOT_HUE_EDGE_MS;
      uint32_t tHue = (t < hueActiveMs) ? t : hueActiveMs;
      float hue = (BOOT_HUE_MAX_DEG * tHue) / hueActiveMs;
      float bright;
      if (t < BOOT_HUE_EDGE_MS) bright = (float)t / BOOT_HUE_EDGE_MS;
      else if (t > BOOT_HUE_SWEEP_MS - BOOT_HUE_EDGE_MS) bright = (float)(BOOT_HUE_SWEEP_MS - t) / BOOT_HUE_EDGE_MS;
      else bright = 1.0f;
      uint8_t r, g, b;
      hsvToRgb(hue, r, g, b);
      pixels.setPixelColor(KEY_TO_LED[i], pixels.Color((uint8_t)(r * bright), (uint8_t)(g * bright), (uint8_t)(b * bright), 0));
    }
    pixels.show();
    delay(BOOT_TICK_MS);
  } while (elapsed < BOOT_PHASE1_TOTAL_MS);
  clearAllLEDs();

  delay(BOOT_POST_PHASE1_DELAY_MS);

  phaseStart = millis();
  uint32_t fadeInSpan = 3 * BOOT_W_STAGGER_MS + BOOT_W_FADE_MS;
  do {
    elapsed = millis() - phaseStart;
    for (int i = 0; i < 4; i++) {
      uint32_t ledStart = (uint32_t)i * BOOT_W_STAGGER_MS;
      float frac;
      if (elapsed < ledStart) frac = 0.0f;
      else if (elapsed >= ledStart + BOOT_W_FADE_MS) frac = 1.0f;
      else frac = (float)(elapsed - ledStart) / BOOT_W_FADE_MS;
      pixels.setPixelColor(KEY_TO_LED[i], pixels.Color(0, 0, 0, (uint8_t)(frac * 255)));
    }
    pixels.show();
    delay(BOOT_TICK_MS);
  } while (elapsed < fadeInSpan);

  delay(BOOT_HOLD_MS);

  phaseStart = millis();
  do {
    elapsed = millis() - phaseStart;
    float frac = 1.0f - (float)elapsed / BOOT_FADE_OUT_MS;
    if (frac < 0) frac = 0;
    for (int i = 0; i < 4; i++) pixels.setPixelColor(i, pixels.Color(0, 0, 0, (uint8_t)(frac * 255)));
    pixels.show();
    delay(BOOT_TICK_MS);
  } while (elapsed < BOOT_FADE_OUT_MS);

  clearAllLEDs();
}

// ---------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  analogReadResolution(12); // RP2040 ADC is 12-bit (0-4095)
  MIDI.begin(MIDI_CHANNEL_OMNI);
  loadSettings();

  pixels.begin();
  pixels.setBrightness(255);
  pixels.show();

  runBootAnimation();

  Serial.println("PiezoKeys ready");
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif
  MIDI.read();

  uint32_t nowUs = micros();
  if (nowUs >= nextSampleAt) {
    nextSampleAt = nowUs + SAMPLE_INTERVAL_US;
    int sample[4];
    for (int i = 0; i < 4; i++) sample[i] = analogRead(PIEZO_PINS[i]);

    // A channel only re-arms once it's actually dropped back below its own
    // trigger threshold -- regardless of how long that takes. Without this,
    // a slow-decaying key (key 1 especially) can still read above its own
    // (now quite low) threshold when REFRACTORY_MS expires, and gets
    // misread as a brand new hit from IDLE even though nothing new happened.
    for (int i = 0; i < 4; i++) {
      if (sample[i] < TRIGGER_THRESHOLD[i]) armedForTrigger[i] = true;
    }

    uint32_t nowMs = millis();

    if (engineState == ENGINE_IDLE) {
      for (int i = 0; i < 4; i++) {
        if (sample[i] >= TRIGGER_THRESHOLD[i] && nowMs >= keyRefractoryUntil[i] && armedForTrigger[i]) {
          for (int c = 0; c < 4; c++) windowPeak[c] = sample[c];
          uint32_t windowMs = (mode == MODE_PLAY) ? PLAY_CHORD_WINDOW_MS : MENU_CHORD_WINDOW_MS;
          windowEndAt = nowMs + windowMs;
          engineState = ENGINE_CAPTURING;
          break;
        }
      }
    } else if (engineState == ENGINE_CAPTURING) {
      for (int c = 0; c < 4; c++) if (sample[c] > windowPeak[c]) windowPeak[c] = sample[c];
      if (nowMs >= windowEndAt) finalizeChord(nowMs);
    } else if (engineState == ENGINE_NOTE_HELD) {
      // A fresh, full-strength hit on ANY key -- the same key(s) already
      // held, a different key, or a whole new combo -- means the user
      // wants to play something new right now. Cut the held note short
      // immediately and start capturing the new hit, instead of waiting
      // for the current note's release to resolve first (which can take
      // a while, especially on key 1) -- needed to actually play at tempo.
      // A held key still needs the higher RETRIGGER_THRESHOLD bar (hold
      // jitter / this same hit's own decay stays well below it, so a
      // crossing there is unambiguous); a different, not-currently-held
      // key just needs its own normal trigger threshold, same as from idle.
      bool retrigger = false;
      if ((nowMs - heldNoteOnAt) >= RETRIGGER_GRACE_MS) {
        for (int i = 0; i < 4; i++) {
          int bar = (heldCombo & (1 << i)) ? RETRIGGER_THRESHOLD : TRIGGER_THRESHOLD[i];
          if (sample[i] >= bar) { retrigger = true; break; }
        }
      }

      if (retrigger) {
        releaseHeldNote(nowMs);
        for (int c = 0; c < 4; c++) windowPeak[c] = sample[c];
        uint32_t windowMs = (mode == MODE_PLAY) ? PLAY_CHORD_WINDOW_MS : MENU_CHORD_WINDOW_MS;
        windowEndAt = nowMs + windowMs;
        engineState = ENGINE_CAPTURING;
      } else {
        bool allBelowRelease = true;
        for (int i = 0; i < 4; i++) {
          if ((heldCombo & (1 << i)) && sample[i] >= RELEASE_THRESHOLD) { allBelowRelease = false; break; }
        }
        if (allBelowRelease) {
          if (releaseCandidateSinceMs == 0) releaseCandidateSinceMs = nowMs;
        } else {
          releaseCandidateSinceMs = 0;
        }

        bool debounceOk = releaseCandidateSinceMs != 0 && (nowMs - releaseCandidateSinceMs) >= RELEASE_DEBOUNCE_MS;
        bool minDurationOk = (nowMs - heldNoteOnAt) >= MIN_NOTE_DURATION_MS;
        bool maxHoldExceeded = (nowMs - heldNoteOnAt) >= MAX_HOLD_MS;

        if ((debounceOk && minDurationOk) || maxHoldExceeded) releaseHeldNote(nowMs);
      }
    }
  }

  uint32_t nowMs = millis();
  if (engineState == ENGINE_MENU_COOLDOWN && nowMs >= menuCooldownEndAt) {
    engineState = ENGINE_IDLE;
  }

  if (mode != MODE_PLAY && (nowMs - lastMenuActivityAt) >= MENU_IDLE_TIMEOUT_MS) {
    toggleMenu(); // idle timeout: drop back to play mode
  }
}
