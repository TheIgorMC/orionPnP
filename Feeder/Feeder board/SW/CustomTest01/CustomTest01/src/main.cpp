#include <Arduino.h>
#include "config.h"
#include "OrionProtocol.cpp"
#include "LedStrip.cpp"
#include <EEPROM.h>

// ===== If selftest needed ======
#if ENABLE_SELFTEST
#include "selftest.cpp"
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
LedStrip ledStrip(2, LED_RGB_PIN); // 2 pixels on the strip

// ====== Protocol variables ======
OrionQueue commandQueue;
OrionPacket incomingPacket;
uint16_t feederAddress = 0x0001; // Feeder address (default)

// ====== Function prototypes ======
void processCommand(const OrionPacket& packet);
uint16_t readFeederAddress();
bool writeFeederAddress(uint16_t address);
void feed(int steps);
void startMotors();
void stopMotors();
void setMotorDirection(bool forward);

void setup() {

  ledStrip.begin();
  ledStrip.startGlow(255, 255, 0, -1); // Start glow on all pixels
  ledStrip.update();

  OrionQueueInit(commandQueue);
  Serial1.begin(115200);
  Serial.begin(19200);

  #if ENABLE_SELFTEST
    runSelfTest();
  #endif

  if(ENABLE_DEBUG)Serial1.println(F("Reading stored data..."));
  feederAddress = readFeederAddress(); // Read address from EEPROM

  delay(1000); // Wait for serial to stabilize
  if(ENABLE_DEBUG)Serial1.println(F("Feeder booting..."));
  

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

// ===== EEPROM Functions ======
uint16_t readFeederAddress() {
  uint16_t address;
  EEPROM.get(EEPROM_ADDR_LOCATION, address);
  if (address == 0x0000 || address > 1023) {
    if(ENABLE_DEBUG)Serial1.println(F("Invalid or unset address, using default."));
    return 0x0000;  // Treat invalid or unset as uninitialized
  }
  if(ENABLE_DEBUG)Serial1.print(F("Feeder address read from EEPROM: "));
  if(ENABLE_DEBUG)Serial1.println(address, HEX);
  return address;
}

bool writeFeederAddress(uint16_t address) {
  if (address == 0x0000 || address > 1023) {
    if(ENABLE_DEBUG)Serial1.println(F("Invalid address, not writing to EEPROM."));
    return false; // Invalid address, do not write
  }
  EEPROM.put(EEPROM_ADDR_LOCATION, address);
  if(ENABLE_DEBUG)Serial1.print(F("Feeder address written to EEPROM: "));
  if(ENABLE_DEBUG)Serial1.println(address, HEX);
  return true;
}

// ====== Motor control functions ======
void feed(int steps) {
  if (steps == 0) {
    stopMotors();
    return;
  }

  bool direction = steps > 0;
  setMotorDirection(direction);
  startMotors();

  uint32_t startTime = millis();

  // Wait before checking optos
  while (millis() - startTime < OPTO_TRIGGER_DELAY_MS) {
    // Ignore opto triggers
  }

  // Reset start time for timeout
  startTime = millis();

  // Watch for opto trigger or timeout
  while (millis() - startTime < OPTO_TIMEOUT_MS) {
    if (digitalRead(OPTO1_PIN) || digitalRead(OPTO2_PIN)) {
      delay(DEBOUNCE_TIME_MS);
      if (digitalRead(OPTO1_PIN) || digitalRead(OPTO2_PIN)) {
        stopMotors();
        return;
      }
    }
  }

  // Timeout fallback
  stopMotors();
#if ENABLE_DEBUG
  Serial1.println(F("Feeder timeout: opto not triggered"));
#endif
}

void startMotors() {
  analogWrite(MOTOR_A_IN1, FEED_SPEED); // Set speed
  digitalWrite(MOTOR_A_IN2, LOW);       // Or reverse if needed
}

void stopMotors() {
  analogWrite(MOTOR_A_IN1, 0);
  analogWrite(MOTOR_A_IN2, 0);
}

void setMotorDirection(bool forward) {
  if (forward) {
    digitalWrite(MOTOR_A_IN2, LOW);
  } else {
    digitalWrite(MOTOR_A_IN2, HIGH); // Or use a different pin if needed
  }
}
