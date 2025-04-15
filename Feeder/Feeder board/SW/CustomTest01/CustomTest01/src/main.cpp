#include <Arduino.h>
#include "config.h"
#include "OrionProtocol.h"
#include "Adafruit_NeoPixel.h"

#if ENABLE_SELFTEST
#include "selftest.h"
#endif

// ====== Constants ======
#define LOAD_TIME 2000 // Time to load tape in milliseconds
#define UNLOAD_TIME 4000 // Time to unload tape in milliseconds
#define FEED_SPEED 255 // Speed for feeding tape (0-255)
#define PEEL_SPEED 255 // Speed for peeling tape (0-255)
#define RATIO 0.5 // Ratio for feed-peel relationship
#define LED_ANIMATION_DELAY 20 // Delay for LED animation in milliseconds
#define LED_ANIMATION_STEPS 50 // Number of steps for LED animation


// ====== Strip initialization ======
Adafruit_NeoPixel strip(2, LED_RGB_PIN, NEO_GRB + NEO_KHZ800); // Declare RGB strip


// ====== Function prototypes ======
void BootAnimation(Adafruit_NeoPixel &strip);
void GlowAnimation(Adafruit_NeoPixel &strip, uint8_t r, uint8_t g, uint8_t b);
void FadeTo(Adafruit_NeoPixel &strip, uint8_t targetR, uint8_t targetG, uint8_t targetB);


void setup() {
  Serial.begin(115200);

  #if ENABLE_SELFTEST
    runSelfTest();
  #endif

  Serial.println(F("Feeder main logic starting..."));
  // Initialize motors, comms, etc.
}

void loop() {
  // Your normal feeder logic goes here
}


// ====== Boot Animation ======
void BootAnimation(Adafruit_NeoPixel &strip) {
  for (int i = 0; i < LED_ANIMATION_STEPS * 2; i++) {
      float angle = (i / (float)LED_ANIMATION_STEPS) * 3.14159; // 0 to PI*2
      float brightness = (sin(angle) + 1.0) / 2.0; // 0 to 1
      uint8_t value = (uint8_t)(brightness * 255);
      uint32_t color = strip.Color(0, 0, value); // Blue
      strip.setPixelColor(0, color);
      strip.setPixelColor(1, color);
      strip.show();
      delay(LED_ANIMATION_DELAY);
  }
}

// ===== Glow Animation ======
void GlowAnimation(Adafruit_NeoPixel &strip, uint8_t r, uint8_t g, uint8_t b, int ledID = -1) {
  for (int i = 0; i < LED_ANIMATION_STEPS * 2; i++) {
      float angle = (i / (float)LED_ANIMATION_STEPS) * 3.14159; // 0 to PI*2
      float brightness = (sin(angle) + 1.0) / 2.0; // 0 to 1
      uint8_t br = (uint8_t)(brightness * 255);
      uint32_t color = strip.Color((r * br) / 255, (g * br) / 255, (b * br) / 255);
      
      if (ledID >= 0 && ledID < strip.numPixels()) {
          strip.setPixelColor(ledID, color);
      } else {
          for (int j = 0; j < strip.numPixels(); j++) {
              strip.setPixelColor(j, color);
          }
      }

      strip.show();
      delay(LED_ANIMATION_DELAY);
  }
}

// ===== Fade to Animation ======
void FadeTo(Adafruit_NeoPixel &strip, uint8_t targetR, uint8_t targetG, uint8_t targetB, int ledID = -1) {
  for (int step = 0; step <= LED_ANIMATION_STEPS; step++) {
      float t = step / (float)LED_ANIMATION_STEPS;

      if (ledID >= 0 && ledID < strip.numPixels()) {
          uint32_t current = strip.getPixelColor(ledID);
          uint8_t r = (uint8_t)((1 - t) * ((current >> 16) & 0xFF) + t * targetR);
          uint8_t g = (uint8_t)((1 - t) * ((current >> 8) & 0xFF) + t * targetG);
          uint8_t b = (uint8_t)((1 - t) * (current & 0xFF) + t * targetB);
          strip.setPixelColor(ledID, strip.Color(r, g, b));
      } else {
          for (int i = 0; i < strip.numPixels(); i++) {
              uint32_t current = strip.getPixelColor(i);
              uint8_t r = (uint8_t)((1 - t) * ((current >> 16) & 0xFF) + t * targetR);
              uint8_t g = (uint8_t)((1 - t) * ((current >> 8) & 0xFF) + t * targetG);
              uint8_t b = (uint8_t)((1 - t) * (current & 0xFF) + t * targetB);
              strip.setPixelColor(i, strip.Color(r, g, b));
          }
      }

      strip.show();
      delay(LED_ANIMATION_DELAY);
  }
}



