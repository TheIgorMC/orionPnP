#include <Arduino.h>
#include "config.h"
#include "selftest.h"
#include <Adafruit_NeoPixel.h>

void runSelfTest()
{
    Serial.println(F("Running self-test..."));

    testFaultLed(); // Test fault LED

    Adafruit_NeoPixel strip(2, LED_RGB_PIN, NEO_GRB + NEO_KHZ800);  // Initialize RGB strip

    testRGB(strip); // Test RGB strip

    testSwitches(strip); // Test switches

    testMotorsAndOpto(); // Test motors and opto sensors
        
    Serial.println(F("Self-test (but not too self) complete."));
    
}

// ====== Test Fault LED ======
void testFaultLed()
{
    pinMode(FAULT_LED_PIN, OUTPUT);
    digitalWrite(FAULT_LED_PIN, !INVERT_FAULT_LED); // Turn on fault LED
    delay(1000);                                    // Wait for 1 second
    digitalWrite(FAULT_LED_PIN, INVERT_FAULT_LED);  // Turn off fault LED
    Serial.println(F("Fault LED test executed."));
}

// ===== Test RGB ======
void testRGB(Adafruit_NeoPixel strip)
{
    Serial.println(F("Testing RGB strip..."));
    strip.begin();
    strip.show(); // Initialize all pixels to 'off'

    strip.setPixelColor(0, strip.Color(255, 0, 0)); // Red
    strip.show();
    delay(1000); // Wait for 1 second

    strip.setPixelColor(0, strip.Color(0, 255, 0)); // Green
    strip.show();
    delay(1000); // Wait for 1 second

    strip.setPixelColor(0, strip.Color(0, 0, 255)); // Blue
    strip.show();
    delay(1000); // Wait for 1 second

    strip.setPixelColor(0, strip.Color(0, 0, 0)); // Off
    strip.show();
    Serial.println(F("RGB strip test executed."));
}

// ====== Test Switches ======
void testSwitches(Adafruit_NeoPixel strip) {
    
    Serial.println(F("Testing switches..."));
    pinMode(SW1_PIN, INPUT_PULLUP); // Set switch pins as input with pull-up
    pinMode(SW2_PIN, INPUT_PULLUP); // Set switch pins as input with pull-up
    
    strip.setPixelColor(0, strip.Color(255, 0, 0)); // Set first LED to red
    strip.setPixelColor(1, strip.Color(255, 0, 0)); // Set second LED to red
    strip.show();
    
    while (digitalRead(SW1_PIN) == SW_ACTIVE_LOW) {
        // Wait for switch 1 to be pressed
    }
    Serial.println(F("Switch 1 pressed."));
    {
        strip.setPixelColor(0, strip.Color(0, 255, 0)); // Set first LED to green
        strip.show();
        delay(1000); // Wait for 1 second
    }
    
    while (digitalRead(SW2_PIN) == SW_ACTIVE_LOW) {
        // Wait for switch 2 to be pressed
    }
    Serial.println(F("Switch 2 pressed."));
    {
        strip.setPixelColor(1, strip.Color(0, 255, 0)); // Set second LED to green
        strip.show();
        delay(1000); // Wait for 1 second
    }
    
    strip.setPixelColor(0, strip.Color(0, 0, 0)); // Set first LED to off
    strip.setPixelColor(1, strip.Color(0, 0, 0)); // Set second LED to off
    strip.show();
    Serial.println(F("Switch test executed."));
    
}

// ====== Test Motors and Opto Sensors ======
void testMotorsAndOpto()
{
    Serial.println(F("Testing motors and optos..."));
    pinMode(MOTOR_A_IN1, OUTPUT); // Set motor pins as output
    pinMode(MOTOR_A_IN2, OUTPUT); // Set motor pins as output
    pinMode(MOTOR_B_IN1, OUTPUT); // Set motor pins as output
    pinMode(MOTOR_B_IN2, OUTPUT); // Set motor pins as output
    pinMode(OPTO1_PIN, INPUT); // Set opto pins as input
    pinMode(OPTO2_PIN, INPUT); // Set opto pins as input
    pinMode(DRV_FAULT_PIN, INPUT); // Set fault pin as input
    pinMode(DRV_SLEEP_PIN, OUTPUT); // Set sleep pin as output

    digitalWrite(DRV_SLEEP_PIN, HIGH); // Wake up the motor driver
    delay(100); // Wait for 100ms to allow the driver to wake up
    if (digitalRead(DRV_FAULT_PIN) == INVERT_FAULT_LED)
    { // Check if fault pin is triggered
        Serial.println(F("Fault detected!"));
        digitalWrite(DRV_SLEEP_PIN, LOW); // Put the driver to sleep
        return; // Exit the function if fault is detected
    }
        
    Serial.println(F("Testing feed motor..."));
    // Drive feed motor forward
    digitalWrite(MOTOR_A_IN1, HIGH); // Set IN1 high
    digitalWrite(MOTOR_A_IN2, LOW);  // Set IN2 low
    delay(2000);                     // Run motor for 2 seconds

    // Stop motor
    digitalWrite(MOTOR_A_IN1, LOW); // Set IN1 low
    digitalWrite(MOTOR_A_IN2, LOW); // Set IN2 low
    if (digitalRead(DRV_FAULT_PIN) == INVERT_FAULT_LED)
    { // Check if fault pin is triggered
        Serial.println(F("Fault detected!"));
        digitalWrite(DRV_SLEEP_PIN, LOW); // Put the driver to sleep
        return; // Exit the function if fault is detected
    }

    // Drive feed motor reverse
    digitalWrite(MOTOR_A_IN1, LOW);
    digitalWrite(MOTOR_A_IN2, HIGH);
    delay(2000);

    // Stop motor A
    digitalWrite(MOTOR_A_IN1, LOW);
    digitalWrite(MOTOR_A_IN2, LOW);
    if (digitalRead(DRV_FAULT_PIN) == INVERT_FAULT_LED) {
        Serial.println(F("Fault detected!"));
        digitalWrite(DRV_SLEEP_PIN, LOW);
        return;
    }
    Serial.println(F("Feed motor test completed."));

    Serial.println(F("Testing peel motor..."));
    // Drive peel motor forward
    digitalWrite(MOTOR_B_IN1, HIGH);
    digitalWrite(MOTOR_B_IN2, LOW);
    delay(2000);

    // Stop motor B
    digitalWrite(MOTOR_B_IN1, LOW);
    digitalWrite(MOTOR_B_IN2, LOW);
    if (digitalRead(DRV_FAULT_PIN) == INVERT_FAULT_LED) {
        Serial.println(F("Fault detected!"));
        digitalWrite(DRV_SLEEP_PIN, LOW);
        return;
    }

    // Drive peel motor reverse
    digitalWrite(MOTOR_B_IN1, LOW);
    digitalWrite(MOTOR_B_IN2, HIGH);
    delay(2000);

    // Stop motor B
    digitalWrite(MOTOR_B_IN1, LOW);
    digitalWrite(MOTOR_B_IN2, LOW);
    if (digitalRead(DRV_FAULT_PIN) == INVERT_FAULT_LED) {
        Serial.println(F("Fault detected!"));
        digitalWrite(DRV_SLEEP_PIN, LOW);
        return;
    }
    Serial.println(F("Peel motor test completed."));


    Serial.println(F("Motor tests executed. Testing optos..."));

    // Test opto sensors moving forward and checking for signal
    digitalWrite(MOTOR_A_IN1, HIGH); // Set IN1 high
    digitalWrite(MOTOR_A_IN2, LOW);  // Set IN2 low

    int count = 0;
    while (count < 5)
    { // Check for signal 5 times
        if (digitalRead(OPTO1_PIN) == INVERT_OPTO)
        { // Check if opto sensor is triggered
            count++;
        }
        delay(500); // Wait for 0.5 seconds
    }
    Serial.println(F("Opto sensor 1 tested."));

    count = 0; // Reset count for opto sensor 2 test
    while (count < 5)
    { // Check for signal 5 times
        if (digitalRead(OPTO2_PIN) == INVERT_OPTO)
        { // Check if opto sensor is triggered
            count++;
        }
        delay(500); // Wait for 0.5 seconds
    }
    Serial.println(F("Opto sensor 2 tested."));

    // Stop motor
    digitalWrite(MOTOR_A_IN1, LOW); // Set IN1 low
    digitalWrite(MOTOR_A_IN2, LOW); // Set IN2 low

    digitalWrite(DRV_SLEEP_PIN, LOW); // Put the driver to sleep
    Serial.println(F("Opto tests executed."));
}


