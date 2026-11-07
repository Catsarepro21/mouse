#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <HijelHID_BLEMouse.h>
#if __has_include("PMW3360Srom.h")
#include "PMW3360Srom.h"
#endif

constexpr uint8_t expander = 0x20;
constexpr int chipSelect = D7;
constexpr uint8_t buttons = 0x07;
constexpr uint32_t sampleIntervalUs = 1000;
constexpr uint32_t reportIntervalUs = 8000;
constexpr uint32_t debounceMs = 5;
HijelBLEMouse mouse("Seashell Mouse", "Custom", 100, 3, false);

Adafruit_NeoPixel pixels(7, D6, NEO_GRB + NEO_KHZ800);
bool opticalReady;
bool previousPairing;
uint8_t previousEncoder = 0xFF;
uint8_t rawButtons;
uint8_t stableButtons;
uint32_t buttonsChanged;
int encoderSteps;
int32_t pendingX, pendingY;

void startSensorTransaction() {
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE3));
  digitalWrite(chipSelect, LOW);
  delayMicroseconds(1);
}

void endSensorTransaction() {
  digitalWrite(chipSelect, HIGH);
  SPI.endTransaction();
}

uint8_t readSensor(uint8_t reg) {
  startSensorTransaction();
  SPI.transfer(reg);
  delayMicroseconds(160);
  const uint8_t value = SPI.transfer(0);
  delayMicroseconds(1);
  endSensorTransaction();
  delayMicroseconds(20);
  return value;
}

void writeSensor(uint8_t reg, uint8_t value) {
  startSensorTransaction();
  SPI.transfer(reg | 0x80);
  SPI.transfer(value);
  delayMicroseconds(35);
  endSensorTransaction();
  delayMicroseconds(145);
}

bool initSensor() {
  digitalWrite(chipSelect, HIGH);
  pinMode(chipSelect, OUTPUT);
  SPI.begin(D2, D10, D3, -1);
  delay(50);
  startSensorTransaction();
  delayMicroseconds(40);
  endSensorTransaction();
  delayMicroseconds(40);
  writeSensor(0x3A, 0x5A);
  delay(50);
  for (uint8_t reg = 0x02; reg <= 0x06; ++reg) readSensor(reg);
  const uint8_t product = readSensor(0x00);
  const uint8_t inverse = readSensor(0x3F);
  Serial.printf("PMW3360 product/inverse: %02X/%02X (expected 42/BD)\n", product, inverse);
  if (product != 0x42 || inverse != 0xBD) return false;
#if __has_include("PMW3360Srom.h")
  writeSensor(0x10, 0);
  writeSensor(0x13, 0x1D);
  delay(10);
  writeSensor(0x13, 0x18);
  startSensorTransaction();
  SPI.transfer(0xE2);
  delayMicroseconds(15);
  for (uint8_t byte : pmw3360Srom) {
    SPI.transfer(byte);
    delayMicroseconds(15);
  }
  endSensorTransaction();
  delayMicroseconds(200);
  const uint8_t revision = readSensor(0x2A);
  Serial.printf("PMW3360 SROM revision: %02X\n", revision);
  if (revision == 0 || revision == 0xFF) return false;
  writeSensor(0x10, 0);
  writeSensor(0x0F, 7);
  return true;
#else
  Serial.println("No PMW3360 SROM header. Click and scroll bring-up only.");
  return false;
#endif
}

int16_t readAxis(uint8_t lowRegister) {
  const uint8_t low = readSensor(lowRegister);
  const uint8_t high = readSensor(lowRegister + 1);
  return static_cast<int16_t>((uint16_t(high) << 8) | low);
}

bool readInputs(uint8_t &value) {
  Wire.beginTransmission(expander);
  Wire.write(0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(expander, uint8_t(1)) != 1) return false;
  value = Wire.read();
  return true;
}

void sampleInputs(bool paired) {
  uint8_t inputs;
  if (!readInputs(inputs)) {
    previousEncoder = 0xFF;
    encoderSteps = 0;
    rawButtons = stableButtons = 0;
    buttonsChanged = millis();
    return;
  }

  const uint8_t pressed = ~inputs & buttons;
  const uint32_t now = millis();
  if (pressed != rawButtons) {
    rawButtons = pressed;
    buttonsChanged = now;
  }
  if (now - buttonsChanged >= debounceMs) stableButtons = rawButtons;

  constexpr int8_t transitions[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
  };
  const uint8_t encoder = (inputs >> 5) & 3;
  if (!paired || previousEncoder == 0xFF || (previousEncoder ^ encoder) == 3) {
    encoderSteps = 0;
  } else {
    encoderSteps += transitions[(previousEncoder << 2) | encoder];

    if (abs(encoderSteps) >= 2) {
      mouse.addScroll(encoderSteps > 0 ? 1 : -1);
      encoderSteps = 0;
    }
  }
  previousEncoder = encoder;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(D4, D5, 400000);
  pixels.begin();
  pixels.clear();
  pixels.show();
  opticalReady = initSensor();
  uint8_t inputs;
  Serial.println(readInputs(inputs) ? "TCA9555 detected" : "TCA9555 not detected");
  mouse.begin();
}

void loop() {
  static uint32_t lastSample, lastReport;
  static uint8_t reportedButtons = 0xFF;
  const uint32_t now = micros();
  const bool paired = mouse.isPaired();
  if (!paired || !previousPairing) {
    reportedButtons = 0xFF;
    pendingX = pendingY = 0;
  }
  if (paired != previousPairing) {
    pixels.fill(paired ? pixels.Color(3, 3, 3) : 0);
    pixels.show();
  }
  previousPairing = paired;

  if (now - lastSample >= sampleIntervalUs) {
    lastSample = now;
    sampleInputs(paired);
    if (opticalReady && (readSensor(0x02) & 0x80)) {
      const int16_t x = readAxis(0x03);
      const int16_t y = readAxis(0x05);
      if (paired) {
        pendingX = constrain(pendingX + x, int32_t(-2048), int32_t(2048));
        pendingY = constrain(pendingY + y, int32_t(-2048), int32_t(2048));
      }
    }
  }

  if (paired && stableButtons != reportedButtons) {
    mouse.setButtons(stableButtons);
    reportedButtons = stableButtons;
  }
  if (now - lastReport >= reportIntervalUs) {
    lastReport = now;
    if (paired && (pendingX || pendingY)) {
      const int16_t x = constrain(pendingX, int32_t(-127), int32_t(127));
      const int16_t y = constrain(pendingY, int32_t(-127), int32_t(127));
      mouse.move(x, y);
      pendingX -= x;
      pendingY -= y;
    }
  }
  delay(1);
}
