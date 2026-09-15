// RP2040 + 4 piezo discs on A0-A3 -> continuous readout for Arduino IDE Serial Plotter
// Also drives 4x SK6812 RGBW pixels on D4, smoothly cycling through R/G/B/W.
//
// Wiring (important for RP2040 - its ADC pins are only 0-3.3V tolerant,
// unlike classic AVR Arduinos which clamp overvoltage internally):
//   Piezo lead 1 -> A0 / A1 / A2 / A3
//   Piezo lead 2 -> GND
//   1M ohm resistor across each piezo's two leads (bias/bleed resistor -
//     keeps the reading centered near 0 instead of floating, and
//     dampens the voltage spike from a hard tap)
//   Optional but recommended clamp diodes (e.g. 1N4148) to protect each ADC
//     pin from voltage spikes outside 0-3.3V:
//       anode -> Ax,  cathode -> 3V3   (clips positive spikes above 3.3V)
//       anode -> GND, cathode -> Ax    (clips negative spikes below 0V)
//
//   SK6812 RGBW data in -> D4 (through a ~330-470 ohm series resistor is
//     recommended), pixel VDD -> 5V, GND -> GND.

#include <Adafruit_NeoPixel.h>

const int PIEZO_PINS[4] = {A0, A1, A2, A3};

// Sample the sensors fast so sharp taps aren't missed between reads...
const uint32_t SAMPLE_INTERVAL_US = 200;   // 0.2ms -> 5000 samples/sec per channel

// ...but only push values to Serial at a rate a human (and the Serial
// Plotter) can actually watch. Between sends we keep the highest sample
// seen per channel, so fast spikes still show up even though the plot
// itself is slow.
const uint32_t PLOT_INTERVAL_MS = 33;      // ~30 updates/sec

uint32_t nextSampleAt = 0;
uint32_t nextPlotAt = 0;
int peaks[4] = {0, 0, 0, 0};

// --- SK6812 RGBW pixels -----------------------------------------------

const int PIXEL_PIN = D4;
const int NUM_PIXELS = 4;
const uint8_t PIXEL_BRIGHTNESS = 51; // 20% of 255

Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRBW + NEO_KHZ800);

struct RGBW {
  uint8_t r, g, b, w;
};

// Order the cycle passes through: Red -> Green -> Blue -> White -> (back to Red)
const RGBW CYCLE_COLORS[4] = {
  {255, 0,   0,   0},
  {0,   255, 0,   0},
  {0,   0,   255, 0},
  {0,   0,   0,   255},
};

const uint32_t COLOR_CYCLE_MS = 8000;               // full 4-color loop duration
const uint32_t PIXEL_UPDATE_INTERVAL_MS = 20;        // ~50 updates/sec, plenty smooth
uint32_t nextPixelUpdateAt = 0;

uint8_t lerp8(uint8_t from, uint8_t to, float frac) {
  return from + (int)((to - from) * frac + 0.5f);
}

void updatePixels() {
  const uint32_t segmentMs = COLOR_CYCLE_MS / 4;
  uint32_t t = millis() % COLOR_CYCLE_MS;
  int segment = t / segmentMs;
  float frac = (t % segmentMs) / (float)segmentMs;

  const RGBW &from = CYCLE_COLORS[segment];
  const RGBW &to = CYCLE_COLORS[(segment + 1) % 4];

  uint32_t color = pixels.Color(
    lerp8(from.r, to.r, frac),
    lerp8(from.g, to.g, frac),
    lerp8(from.b, to.b, frac),
    lerp8(from.w, to.w, frac)
  );

  for (int i = 0; i < NUM_PIXELS; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

// ------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  analogReadResolution(12); // RP2040 ADC is 12-bit (0-4095)

  pixels.begin();
  pixels.setBrightness(PIXEL_BRIGHTNESS);
  pixels.show(); // all off until the first updatePixels() call
}

void loop() {
  uint32_t nowUs = micros();
  if (nowUs >= nextSampleAt) {
    nextSampleAt = nowUs + SAMPLE_INTERVAL_US;
    for (int i = 0; i < 4; i++) {
      int value = analogRead(PIEZO_PINS[i]);
      if (value > peaks[i]) peaks[i] = value;
    }
  }

  uint32_t nowMs = millis();

  if (nowMs >= nextPlotAt) {
    nextPlotAt = nowMs + PLOT_INTERVAL_MS;

    // "Ceiling" and "Floor" are constant at the ADC's true 0-4095 range.
    // Plotting them alongside the peaks pins the graph's autoscale to the
    // full range, so you can actually see how much headroom you're using
    // instead of the plot rescaling to fit whatever just came in.
    Serial.print("Peak0:");
    Serial.print(peaks[0]);
    Serial.print(",Peak1:");
    Serial.print(peaks[1]);
    Serial.print(",Peak2:");
    Serial.print(peaks[2]);
    Serial.print(",Peak3:");
    Serial.print(peaks[3]);
    Serial.print(",Ceiling:4095,Floor:0");
    Serial.println();

    for (int i = 0; i < 4; i++) peaks[i] = 0; // reset the hold for the next window
  }

  if (nowMs >= nextPixelUpdateAt) {
    nextPixelUpdateAt = nowMs + PIXEL_UPDATE_INTERVAL_MS;
    updatePixels();
  }
}
