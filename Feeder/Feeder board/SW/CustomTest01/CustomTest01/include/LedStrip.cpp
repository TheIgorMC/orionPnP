// LedStrip.cpp
#include "LedStrip.h"
#include <Arduino.h>
#include <math.h>

LedStrip::LedStrip(uint8_t numPixels, uint8_t pin)
  : strip(numPixels, pin, NEO_GRB + NEO_KHZ800), numPixels(numPixels) {
    pixels = new PixelState[numPixels];
}

void LedStrip::begin() {
    strip.begin();
    strip.show();
}

void LedStrip::startGlow(uint8_t r, uint8_t g, uint8_t b, int ledID) {
    if (ledID < 0 || ledID >= numPixels) return;
    pixels[ledID].glowActive = true;
    pixels[ledID].glowR = r;
    pixels[ledID].glowG = g;
    pixels[ledID].glowB = b;
    pixels[ledID].glowStep = 0;
    pixels[ledID].glowLastUpdate = millis();
}

void LedStrip::stopGlow(int ledID) {
    if (ledID < 0 || ledID >= numPixels) return;
    pixels[ledID].glowActive = false;
    strip.setPixelColor(ledID, 0);
    strip.show();
}

void LedStrip::fadeTo(uint8_t r, uint8_t g, uint8_t b, int ledID) {
    if (ledID < 0 || ledID >= numPixels) return;
    pixels[ledID].fadeActive = true;
    pixels[ledID].fadeTargetR = r;
    pixels[ledID].fadeTargetG = g;
    pixels[ledID].fadeTargetB = b;
    pixels[ledID].fadeStep = 0;
    pixels[ledID].fadeLastUpdate = millis();
}

void LedStrip::startBlink(uint8_t r, uint8_t g, uint8_t b, uint16_t duration, int ledID) {
    if (ledID < 0 || ledID >= numPixels) return;
    pixels[ledID].blinkActive = true;
    pixels[ledID].blinkStartTime = millis();
    pixels[ledID].blinkDuration = duration;
    pixels[ledID].blinkR = r;
    pixels[ledID].blinkG = g;
    pixels[ledID].blinkB = b;
    strip.setPixelColor(ledID, strip.Color(r, g, b));
    strip.show();
}

void LedStrip::update() {
    uint32_t now = millis();
    for (int i = 0; i < numPixels; i++) {
        // Glow
        if (pixels[i].glowActive && now - pixels[i].glowLastUpdate >= DELAY_MS) {
            pixels[i].glowLastUpdate = now;
            float angle = (pixels[i].glowStep / (float)STEPS) * 3.14159;
            float brightness = (sin(angle) + 1.0) / 2.0;
            uint8_t br = (uint8_t)(brightness * 255);
            uint32_t color = strip.Color((pixels[i].glowR * br) / 255, (pixels[i].glowG * br) / 255, (pixels[i].glowB * br) / 255);
            strip.setPixelColor(i, color);
            pixels[i].glowStep = (pixels[i].glowStep + 1) % (STEPS * 2);
        }

        // Fade
        if (pixels[i].fadeActive && now - pixels[i].fadeLastUpdate >= DELAY_MS) {
            pixels[i].fadeLastUpdate = now;
            float t = pixels[i].fadeStep / (float)STEPS;
            uint32_t current = strip.getPixelColor(i);
            uint8_t r = (uint8_t)((1 - t) * ((current >> 16) & 0xFF) + t * pixels[i].fadeTargetR);
            uint8_t g = (uint8_t)((1 - t) * ((current >> 8) & 0xFF) + t * pixels[i].fadeTargetG);
            uint8_t b = (uint8_t)((1 - t) * (current & 0xFF) + t * pixels[i].fadeTargetB);
            strip.setPixelColor(i, strip.Color(r, g, b));
            pixels[i].fadeStep++;
            if (pixels[i].fadeStep > STEPS) {
                pixels[i].fadeActive = false;
            }
        }

        // Blink
        if (pixels[i].blinkActive && now - pixels[i].blinkStartTime >= pixels[i].blinkDuration) {
            strip.setPixelColor(i, 0);
            pixels[i].blinkActive = false;
        }
    }
    strip.show();
}
