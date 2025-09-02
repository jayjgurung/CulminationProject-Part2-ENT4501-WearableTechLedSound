#include <IRremote.h>
#include <FastLED.h>
 
#define NUM_LEDS 50 //number of addressable LEDs
#define DATA_PIN 8  // Addressable LED data pin
// Time scaling factors for each component
#define TIME_FACTOR_HUE 60
#define TIME_FACTOR_SAT 100
#define TIME_FACTOR_VAL 100

// Defining the array of LEDs
CRGB leds[NUM_LEDS];



const int micPin = A0;          // Mic connected to analog pin A0
const int IR_RECEIVE_PIN = 7;   // IR receiver connected to pin 7
const int LED1_PIN = 2;         // LED1 connected to pin 2
const int LED2_PIN = 3;         // LED2 connected to pin 3
const int LED3_PIN = 4;         // LED3 connected to pin 4
const int LED4_PIN = 5;         // LED4 connected to pin 5
const int LED5_PIN = 6;         // LED5 connected to pin 6

long int lastReceivedValue = 0; // Variable to store the last received IR value
const int baseline = 500;      // Baseline for sound detection (adjust as needed)
const int baselinefx3 = 990; //baseline threshold for fx3

bool micControlActive = false;  // Flag to indicate microphone-controlled LED mode
bool fx1 = false;
bool fx2 = false;
bool fx3 = false;

void setup() {
  Serial.begin(9600);
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS).setRgbw(RgbwDefault());
  FastLED.setBrightness(128);  // Set global brightness to 50%
  delay(2000);  // If something ever goes wrong this delay will allow upload.

  // Initialize IR receiver
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  // Set up LED pins as output
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);
  pinMode(LED4_PIN, OUTPUT);
  pinMode(LED5_PIN, OUTPUT);

}

void loop() {
  // Check if IR data is received
  if (IrReceiver.decode()) {
    long int receivedValue = IrReceiver.decodedIRData.decodedRawData;

    // Ignore signal `0` entirely
    if (receivedValue == 0) {
      IrReceiver.resume();  // Prepare for the next IR signal
      return;
    }

    // Handle power off
    if (receivedValue == 0xE916FF00) {  // Remote button 'Power OFF'
      micControlActive = false;
      fx1 = false;
      fx2 = false;
      fx3 = false;  // stops all led animation

      turnOffAllLEDs();

      fill_solid(leds, NUM_LEDS, CRGB::Black); // Turn off all addressable LEDs
      FastLED.show();

      lastReceivedValue = 0; // Reset lastReceivedValue
    } else if (receivedValue != lastReceivedValue) {
      lastReceivedValue = receivedValue;  // Update last received value

      // Turn off all LEDs before turning on the selected one
      turnOffAllLEDs();

      switch (receivedValue) {
        case 0xF30CFF00:  // Remote button '1'
          micControlActive = false;
          fx1 = true;  // Start Cylon animation
          fx2 = false;
          fx3 = false;
          break;

        case 0xE718FF00:  // Remote button '2'
          micControlActive = false;
          fx2 = true;
          fx1 = false;
          fx3 = false;
          digitalWrite(LED2_PIN, HIGH);
          break;

        case 0xA15EFF00:  // Remote button '3'
          micControlActive = false;
          fx1 = false;
          fx2 = false;
          fx3 = true;
          digitalWrite(LED3_PIN, HIGH);
          break;

        case 0xF708FF00:  // Remote button '4'
          micControlActive = false;
          fx1 = false;
          digitalWrite(LED4_PIN, HIGH);
          break;

        case 0xE31CFF00:  // Remote button '5'
          micControlActive = false;
          fx1 = false;
          digitalWrite(LED5_PIN, HIGH);
          break;

        case 0xA55AFF00:  // Remote Button '6'
          micControlActive = true;
          fx1 = false; // Disable Cylon when mic control is active
          break;

        default:
          break;
      }
    }

    // Prepare for the next IR signal
    IrReceiver.resume();
  }

  // Run microphone-controlled LED logic
  if (micControlActive) {
    int micReading = analogRead(micPin);  // Read the microphone analog value
    Serial.print("Mic Reading: ");
    Serial.println(micReading);  // Print the mic reading to the serial monitor
    
    if (micReading < baseline) {
      turnOnAllLEDs();
    } else {
      turnOffAllLEDs();
    }
    delay(10);  // Add a short delay to stabilize readings
  }

  //  Cylon effect and LED indicator
  if (fx1) {
    uint32_t ms = millis();
    digitalWrite(LED1_PIN, HIGH);
    for(int i = 0; i < NUM_LEDS; i++) {
        // Use different noise functions for each LED and each color component
        uint8_t hue = inoise16(ms * TIME_FACTOR_HUE, i * 1000, 0) >> 8;
        uint8_t sat = inoise16(ms * TIME_FACTOR_SAT, i * 2000, 1000) >> 8;
        uint8_t val = inoise16(ms * TIME_FACTOR_VAL, i * 3000, 2000) >> 8;
        
        // Map the noise to full range for saturation and value
        sat = map(sat, 0, 255, 30, 255);
        val = map(val, 0, 255, 100, 255);
        
        leds[i] = CHSV(hue, sat, val);
    }
    FastLED.setBrightness(128);
    FastLED.show();
    Serial.println(lastReceivedValue);
    if (IrReceiver.decode()) {
        long int receivedValue = IrReceiver.decodedIRData.decodedRawData;

        // Ignore signal `0` entirely
        if (receivedValue != 0 && receivedValue != 0xF30CFF00) {
          fx1 = false;  // Prepare for the next IR signal
            // Turn off all LEDs
            FastLED.clear(); // Turn off all LEDs
            FastLED.show();
            digitalWrite(LED1_PIN, LOW);

          // return;
        }
      // Prepare for the next IR signal
      IrReceiver.resume();
    }
  }

  if (fx2) {
    for (int i = 0; i < NUM_LEDS; i++) {
      if (!fx2) break; // Exit the loop immediately if fx2 is false

      FastLED.clear(); // Turn off all LEDs

      // Light up 5 LEDs at a time
      for (int j = 0; j < 10; j++) {
        if (!fx2) break; // Exit the inner loop if fx2 is false

        int ledIndex = (i + j) % NUM_LEDS; // Wrap around the strip
        leds[ledIndex] = CRGB::Blue;       // Set the LED to blue

        Serial.println(lastReceivedValue);

        if (IrReceiver.decode()) {
          long int receivedValue = IrReceiver.decodedIRData.decodedRawData;

          // Ignore signal `0` entirely
          if (receivedValue != 0 && receivedValue != 0xE718FF00) {
            fx2 = false; // Set fx2 to false to stop the animation

            // Turn off all LEDs
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            FastLED.show();

            break; // Exit the inner loop
          }

          // Prepare for the next IR signal
          IrReceiver.resume();
        }
      }

      FastLED.show(); // Update the LEDs
      delay(100);     // Wait for 100ms
    }
  }

  if (fx3) {
    int micReading = analogRead(micPin); // Read the microphone input
    Serial.print("Mic Reading: ");
    Serial.println(micReading); // Debug microphone value

    for (int i = 0; i < NUM_LEDS; i++) {
      // Generate random RGB values
      uint8_t red = random(0, 256);
      uint8_t green = random(0, 256);
      uint8_t blue = random(0, 256);

      // Set LED colors based on microphone input level
      if (micReading < baselinefx3) {
        leds[i] = CRGB(red, green, blue); // Assign random colors if sound is above baseline
      } else {
        leds[i] = CRGB::Black; // Turn off LEDs if sound is below baseline
      }
    }

    // Update LEDs with brightness based on mic input
    uint8_t brightness = map(micReading, baseline, 1023, 50, 255); // Map mic value to brightness range
    FastLED.setBrightness(brightness);

    FastLED.show();
    delay(50); // Small delay for stability

    // Check for IR input to disable fx3
    if (IrReceiver.decode()) {
      long int receivedValue = IrReceiver.decodedIRData.decodedRawData;

      if (receivedValue != 0 && receivedValue != 0xA15EFF00) { // Stop fx3 on receiving a different IR signal
        fx3 = false;

        // Turn off all LEDs
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
      }

      // Resume IR receiver for next signal
      IrReceiver.resume();
    }
  }

}

// Helper function to turn off all LEDs
void turnOffAllLEDs() {
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  digitalWrite(LED3_PIN, LOW);
  digitalWrite(LED4_PIN, LOW);
  digitalWrite(LED5_PIN, LOW);
}

// Helper function to turn on all LEDs
void turnOnAllLEDs() {
  digitalWrite(LED1_PIN, HIGH);
  digitalWrite(LED2_PIN, HIGH);
  digitalWrite(LED3_PIN, HIGH);
  digitalWrite(LED4_PIN, HIGH);
  digitalWrite(LED5_PIN, HIGH);
}