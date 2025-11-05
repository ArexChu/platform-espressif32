/*
 * Blink with Serial Output
 * Turns on an LED on for one second,
 * then off for one second, repeatedly.
 * Prints LED status to the Serial Monitor.
 */

#include <Arduino.h>

#define LED_PIN 13

void setup()
{
  // Initialize LED digital pin as an output.
  pinMode(LED_PIN, OUTPUT);

  // Initialize serial communication at 9600 bits per second
  Serial.begin(115200);
  Serial.println("Blink sketch started!");
}

void loop()
{
  // Turn the LED on (HIGH is the voltage level)
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED is ON");
  // Wait for a second
  delay(1000);

  // Turn the LED off by making the voltage LOW
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED is OFF");
  // Wait for a second
  delay(1000);
}
