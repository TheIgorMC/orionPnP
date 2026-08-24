// LedStrip.h
#ifndef LED_STRIP_H
#define LED_STRIP_H

#include <Adafruit_NeoPixel.h>

class LedStrip {
public:
    LedStrip(uint8_t numPixels, uint8_t pin);
    void begin();
    void update();

    void startGlow(uint8_t r, uint8_t g, uint8_t b, int ledID);
    void stopGlow(int ledID);

    void fadeTo(uint8_t r, uint8_t g, uint8_t b, int ledID);
    void startBlink(uint8_t r, uint8_t g, uint8_t b, uint16_t duration, int ledID);

private:
    Adafruit_NeoPixel strip;

    struct PixelState {
        // Glow
        bool glowActive = false;
        uint32_t glowLastUpdate = 0;
        int glowStep = 0;
        uint8_t glowR = 0, glowG = 0, glowB = 0;

        // Fade
        bool fadeActive = false;
        uint32_t fadeLastUpdate = 0;
        int fadeStep = 0;
        uint8_t fadeTargetR = 0, fadeTargetG = 0, fadeTargetB = 0;

        // Blink
        bool blinkActive = false;
        uint32_t blinkStartTime = 0;
        uint16_t blinkDuration = 0;
        uint8_t blinkR = 0, blinkG = 0, blinkB = 0;
    };

    static constexpr uint8_t STEPS = 50;
    static constexpr uint8_t DELAY_MS = 20;

    PixelState* pixels;
    uint8_t numPixels;
};

#endif
