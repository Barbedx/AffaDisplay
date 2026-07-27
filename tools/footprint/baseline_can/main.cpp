#include <Arduino.h>
#include <esp32_can.h>
void setup() { Serial.begin(115200); CAN0.begin(500000); }
void loop() {}
