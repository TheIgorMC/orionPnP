/*
 * Project: Custom Feeder Control
 * File: main.cpp
 * Description: This file contains the main implementation for the feeder control system. 
 *              It includes motor control, LED strip animations, command processing, 
 *              and EEPROM address management.
 * 
 * Author: Giovanni C.
 * 
 * Board: OrionPnP_Feeder V0.2_
 * 
 * Features:
 *   - Motor control for feeding and peeling tape
 *   - LED animations for status indication
 *   - Command processing via Orion Protocol
 *   - EEPROM-based address storage
 *   - Self-test functionality (optional)
 * 
 * Notes:
 *   - Ensure proper wiring of motors, LEDs, and switches.
 *   - Adjust constants in config.h as per hardware setup.
 * 
 */


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
#define LOAD_STEPS 20 // Number of steps to load tape
#define UNLOAD_STEPS 40 // Number of steps to unload tape
#define FEED_SPEED 255 // Speed for feeding tape (0-255)
#define RATIO 1 // Ratio for feed-peel relationship
#define LED_ANIMATION_DELAY 20 // Delay for LED animation in milliseconds
#define LED_ANIMATION_STEPS 50 // Number of steps for LED animation


// ====== Strip initialization ======
LedStrip ledStrip(2, LED_RGB_PIN); // 2 pixels on the strip

// ====== Protocol variables ======
OrionQueue commandQueue;
OrionPacket incomingPacket;
uint16_t feederAddress = 0x0000; // Feeder address (default is no address)

// ====== Function prototypes ======
void processCommand(const OrionPacket& packet);
uint16_t readFeederAddress();
bool writeFeederAddress(uint16_t address);
void feed(int steps);
void startMotors();
void stopMotors();
void setMotorDirection(bool forward);
enum PressType { NO_PRESS, SHORT_PRESS, LONG_PRESS };
PressType checkSwitch(uint8_t pin);


void setup() {

  ledStrip.begin();
  ledStrip.startGlow(255, 255, 0, -1); // Start glow on all pixels
  ledStrip.update();

  OrionQueueInit(commandQueue);
  Serial1.begin(115200);
  Serial.begin(19200);  
  delay(1000); // Wait for serial to stabilize
  if(ENABLE_DEBUG)Serial1.println(F("Feeder booting..."));

  // ===== Pin initialization ======
  pinMode(MOTOR_A_IN1, OUTPUT); // Set motor pins as output
  pinMode(MOTOR_A_IN2, OUTPUT); // Set motor pins as output
  pinMode(MOTOR_B_IN1, OUTPUT); // Set motor pins as output
  pinMode(MOTOR_B_IN2, OUTPUT); // Set motor pins as output
  pinMode(OPTO1_PIN, INPUT); // Set opto pins as input
  pinMode(OPTO2_PIN, INPUT); // Set opto pins as input
  pinMode(DRV_FAULT_PIN, INPUT); // Set fault pin as input
  pinMode(DRV_SLEEP_PIN, OUTPUT); // Set sleep pin as output
  pinMode(RS485_RE_DE, OUTPUT); // Set RS485 RE+DE control pin as output
  pinMode(FAULT_LED_PIN, OUTPUT); // Set fault LED pin as output
  pinMode(SW1_PIN, SW_ACTIVE_LOW ? INPUT_PULLUP : INPUT); // Set switch pins as input with pull-up
  pinMode(SW2_PIN, SW_ACTIVE_LOW ? INPUT_PULLUP : INPUT); // Set switch pins as input with pull-up
  pinMode(LED_RGB_PIN, OUTPUT); // Set LED RGB pin as output

  digitalWrite(DRV_SLEEP_PIN, LOW); // Put driver to sleep
  digitalWrite(RS485_RE_DE, LOW); // Set RS485 to receive mode
  digitalWrite(FAULT_LED_PIN, INVERT_FAULT_LED ? LOW : HIGH); // Set fault LED state

  #if ENABLE_SELFTEST
    if(ENABLE_DEBUG)Serial1.println(F("Running self-test..."));
    runSelfTest();
  #endif

  if(ENABLE_DEBUG)Serial1.println(F("Reading stored data..."));
  feederAddress = readFeederAddress(); // Read address from EEPROM

  if(ENABLE_DEBUG)Serial1.print(F("Zeroing motors..."));
  feed(-1); // Zero motors to closest opto sensor

  ledStrip.stopGlow(-1); // Stop glow on all pixels
  ledStrip.startGlow(0, 255, 0, 0); // Start glow on first pixel (green)
  ledStrip.update();

  if(ENABLE_DEBUG)Serial1.println(F("Feeder ready."));
  
}

void loop()
{
  // ====== Update LED strip ======
  ledStrip.update();

  // ====== Check for switch presses ======
  PressType switch1 = checkSwitch(SW1_PIN);
  PressType switch2 = checkSwitch(SW2_PIN);

  if(switch1 == SHORT_PRESS){
    feed(1); // Feed forward
  }
  else if(switch1 == LONG_PRESS){
    feed(LOAD_STEPS); // Load tape
  }
  else if(switch2 == SHORT_PRESS){
    feed(-1); // Feed backward
  }
  else if(switch2 == LONG_PRESS){
    feed(-UNLOAD_STEPS); // Unload tape
  }
  
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
        else if(incomingPacket.command != CMD_ADDR_ASSIGN )
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
            // Assign address to this device
            if (packet.payload == 0x0000) {
                Serial1.println(F("Invalid address assignment."));
                break;
            }
            if (writeFeederAddress(packet.payload)) {
                feederAddress = packet.payload;
                Serial1.print(F("Address assigned: "));
                Serial1.println(feederAddress, HEX);
                ledStrip.startBlink(0, 255, 0, 1000, 1); // Blink green for success
                ledStrip.update();
                OrionPacket confirm = OrionMakePacket(ORION_HOST_ADDR, packet.commandId, CMD_ADDR_CONFIRM, feederAddress);
                OrionSendPacket(Serial, confirm);
            }
            else {
                Serial1.println(F("Failed to write address to EEPROM."));
            }
            break;        
        case CMD_FEED_FORWARD:
            ledStrip.startBlink(0, 0, 255, 50, 1); // Blink blue for feed forward
            ledStrip.update();
            feed(packet.payload); // Feed forward
            break;
        case CMD_FEED_BACKWARD:
            ledStrip.startBlink(255, 0, 255, 50, 1); // Blink purple for feed forward
            ledStrip.update();
            feed(-1 * packet.payload); // Feed backward
            break;
        case CMD_LOAD_TAPE:
            ledStrip.startBlink(255, 0, 255, 500, -1); // Blink purple for load tape
            ledStrip.update();
            feed(LOAD_STEPS); // Feed forward
            break;
        case CMD_UNLOAD_TAPE:
            ledStrip.startBlink(0, 0, 255, 500, -1); // Blink blue for unload tape
            ledStrip.update();
            feed(-UNLOAD_STEPS); // Feed backward
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

  digitalWrite(DRV_SLEEP_PIN, HIGH); // Wake up the motor driver

  bool direction = steps > 0;
  uint8_t stepsToDo = abs(steps);
  setMotorDirection(direction);
  startMotors();

  delay(OPTO_TRIGGER_DELAY_MS);

  uint8_t completedSteps = 0;
  int8_t lastTriggered = -1; // -1 = none, 0 = opto1, 1 = opto2

  while (completedSteps < stepsToDo) {
    uint32_t stepStartTime = millis();
    while (millis() - stepStartTime < OPTO_TIMEOUT_MS) {
      bool opto1 = digitalRead(OPTO1_PIN);
      bool opto2 = digitalRead(OPTO2_PIN);

      // If we haven't triggered anything yet
      if (lastTriggered == -1) {
        if (opto1) {
          delay(DEBOUNCE_TIME_MS);
          if (digitalRead(OPTO1_PIN)) {
            lastTriggered = 0;
            completedSteps++;
            break;
          }
        } else if (opto2) {
          delay(DEBOUNCE_TIME_MS);
          if (digitalRead(OPTO2_PIN)) {
            lastTriggered = 1;
            completedSteps++;
            break;
          }
        }
      }

      // Already triggered one — now expect the other
      else if (lastTriggered == 0 && opto2) {
        delay(DEBOUNCE_TIME_MS);
        if (digitalRead(OPTO2_PIN)) {
          lastTriggered = 1;
          completedSteps++;
          break;
        }
      } else if (lastTriggered == 1 && opto1) {
        delay(DEBOUNCE_TIME_MS);
        if (digitalRead(OPTO1_PIN)) {
          lastTriggered = 0;
          completedSteps++;
          break;
        }
      }
    }

    if (millis() - stepStartTime >= OPTO_TIMEOUT_MS) {
      stopMotors();
      #if ENABLE_DEBUG
        Serial1.println(F("Feeder timeout during step sequence"));
      #endif
      return;
    }
  }

  stopMotors();
}

void startMotors() {
  // Feed motor
  bool feedForward = digitalRead(MOTOR_A_IN2) == LOW; // direction set previously
  if (feedForward) {
    digitalWrite(MOTOR_A_IN1, HIGH);
    analogWrite(MOTOR_A_IN2, FEED_SPEED);
  } else {
    digitalWrite(MOTOR_A_IN1, LOW);
    analogWrite(MOTOR_A_IN2, FEED_SPEED);
  }

  // Peel motor
  bool peelForward = digitalRead(MOTOR_B_IN2) == LOW;
  uint8_t peelSpeed = (uint8_t)(FEED_SPEED * RATIO);
  if (peelForward) {
    digitalWrite(MOTOR_B_IN1, HIGH);
    analogWrite(MOTOR_B_IN2, peelSpeed);
  } else {
    digitalWrite(MOTOR_B_IN1, LOW);
    analogWrite(MOTOR_B_IN2, peelSpeed);
  }
}

void stopMotors() {
  // Brake both motors by setting both inputs HIGH
  digitalWrite(MOTOR_A_IN1, HIGH);
  digitalWrite(MOTOR_A_IN2, HIGH);
  digitalWrite(MOTOR_B_IN1, HIGH);
  digitalWrite(MOTOR_B_IN2, HIGH);

  delay(100); // Allow time for braking
  digitalWrite(DRV_SLEEP_PIN, LOW); // Put driver to sleep

}

void setMotorDirection(bool forward) {
  // Apply inversion flags
  bool feedDir = forward ^ INVERT_DIR_A;
  bool peelDir = forward ^ INVERT_DIR_B;

  digitalWrite(MOTOR_A_IN2, feedDir ? HIGH : LOW);
  digitalWrite(MOTOR_B_IN2, peelDir ? HIGH : LOW);
}

// ====== Switch check function ======
PressType checkSwitch(uint8_t pin) {
  static bool waitingForRelease = false;
  static bool longPressTriggered = false;
  static uint32_t pressStartTime = 0;
  static bool lastStableState = HIGH;

  bool rawState = digitalRead(pin);

  // Debounce
  static uint32_t lastChangeTime = 0;
  static bool lastRead = HIGH;

  if (rawState != lastRead) {
    lastChangeTime = millis();
    lastRead = rawState;
  }

  if (millis() - lastChangeTime < SW_DEBOUNCE_TIME_MS) {
    return NO_PRESS;
  }

  bool debouncedState = lastRead;

  // Pressed
  if (!waitingForRelease && debouncedState == LOW && lastStableState == HIGH) {
    pressStartTime = millis();
    waitingForRelease = true;
    longPressTriggered = false;
  }

  // While holding, check for long press trigger
  if (waitingForRelease && !longPressTriggered && debouncedState == LOW &&
      millis() - pressStartTime >= SW_TRIGGER_DELAY_MS) {
    longPressTriggered = true;
    return LONG_PRESS;
  }

  // Released
  if (waitingForRelease && debouncedState == HIGH && lastStableState == LOW) {
    waitingForRelease = false;
    if (!longPressTriggered) return SHORT_PRESS;
  }

  lastStableState = debouncedState;
  return NO_PRESS;
}
