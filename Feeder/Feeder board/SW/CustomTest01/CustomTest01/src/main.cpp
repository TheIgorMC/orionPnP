#include <Arduino.h>
#include "Adafruit_NeoPixel.h"
#include "config.h"
#include "OrionProtocol.cpp"
#include <EEPROM.h>

// ===== If selftest needed ======
#if ENABLE_SELFTEST
#include "selftest.h"
#endif


// ====== Constants ======
#define LOAD_TIME 2000 // Time to load tape in milliseconds
#define UNLOAD_TIME 4000 // Time to unload tape in milliseconds
#define FEED_SPEED 255 // Speed for feeding tape (0-255)
#define PEEL_SPEED 255 // Speed for peeling tape (0-255)
#define RATIO 1 // Ratio for feed-peel relationship
#define LED_ANIMATION_DELAY 20 // Delay for LED animation in milliseconds
#define LED_ANIMATION_STEPS 50 // Number of steps for LED animation


// ====== Strip initialization ======
Adafruit_NeoPixel strip(2, LED_RGB_PIN, NEO_GRB + NEO_KHZ800); // Declare RGB strip

// ====== Protocol variables ======
OrionQueue commandQueue;
OrionPacket incomingPacket;
int feederAddress = 0x0001; // Feeder address (default)

// ====== Function prototypes ======
void BootAnimation(Adafruit_NeoPixel &strip);
void GlowAnimation(Adafruit_NeoPixel &strip, uint8_t r, uint8_t g, uint8_t b);
void FadeTo(Adafruit_NeoPixel &strip, uint8_t targetR, uint8_t targetG, uint8_t targetB);


void setup() {

  OrionQueueInit(commandQueue);
  Serial1.begin(115200);
  Serial.begin(19200);

  #if ENABLE_SELFTEST
    runSelfTest();
  #endif

  Serial1.println(F("Feeder main logic starting..."));
  // Initialize motors, comms, etc.
}

void loop()
{
  // ====== Check for available packets ======
  if (Serial.available() >= ORION_PACKET_SIZE)
  {
    // ====== Receive packet ======
    if (OrionReceivePacket(Serial, incomingPacket))
    {
      // Check if the packet is addressed to this device
      if (incomingPacket.address == feederAddress || incomingPacket.address == 0x0000)
      {
        // Start ack timer
        uint32_t rxTime = millis();

        if (!OrionQueueEnqueue(commandQueue, incomingPacket))
        {
          // Buffer full, notify sender
          OrionPacket nack = OrionMakePacket(feederAddress, incomingPacket.commandId, CMD_NACK, ACK_BUFFER_FULL);
          if (millis() - rxTime <= ORION_ACK_TIMEOUT_MS / 2) {
            OrionSendPacket(Serial, nack);
          }
        }
        else
        {
          // Acknowledge queueing
          OrionPacket ack = OrionMakePacket(feederAddress, incomingPacket.commandId, CMD_ACK, ACK_QUEUED);
          if (millis() - rxTime <= ORION_ACK_TIMEOUT_MS / 2) {
            OrionSendPacket(Serial, ack);
          }
        }
      }
    }

    // ===== Process commands ======
    OrionPacket nextCommand;
    if (!OrionQueueIsEmpty(commandQueue) && OrionQueueDequeue(commandQueue, nextCommand))
    {
      processCommand(nextCommand);
    }
  }


}

// ====== Command processor ======
void processCommand(const OrionPacket& packet) {
    switch (packet.command) {
        case CMD_ADDR_ASSIGN:
            // Handle address assignment
            break;
        case CMD_ADDR_CONFIRM:
            // Handle address confirmation
            break;
        case CMD_POLL:
            // Handle polling
            break;
        case CMD_FEED_FORWARD:
            // Handle feed forward command
            break;
        case CMD_FEED_BACKWARD:
            // Handle feed backward command
            break;
        case CMD_LOAD_TAPE:
            // Handle load tape command
            break;
        case CMD_UNLOAD_TAPE:
            // Handle unload tape command
            break;
        default:
            Serial1.println(F("Unknown command received."));
    }
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

// ===== EEPROM Functions ======
uint16_t readFeederAddress() {
  uint16_t address;
  EEPROM.get(EEPROM_ADDR_LOCATION, address);
  if (address == 0x0000 || address > 1023) {
    return 0x0000;  // Treat invalid or unset as uninitialized
  }
  return address;
}


