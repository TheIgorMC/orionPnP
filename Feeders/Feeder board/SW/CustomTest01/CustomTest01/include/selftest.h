#ifndef SELFTEST_H
#define SELFTEST_H

void runSelfTest();
void testFaultLed();
void testRGB(Adafruit_NeoPixel strip);
void testSwitches(Adafruit_NeoPixel strip);
void testMotorsAndOpto();

#endif
