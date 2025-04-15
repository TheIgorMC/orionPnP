#include <Arduino.h>
#include "config.h"
#include "OrionProtocol.h"

#if ENABLE_SELFTEST
#include "selftest.h"
#endif

// ====== Constants ======
#define LOAD_TIME 2000 // Time to load tape in milliseconds
#define UNLOAD_TIME 4000 // Time to unload tape in milliseconds
#define FEED_SPEED 255 // Speed for feeding tape (0-255)
#define PEEL_SPEED 255 // Speed for peeling tape (0-255)
#define RATIO 0.5 // Ratio for feed-peel relationship

// ====== Function prototypes ======



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
