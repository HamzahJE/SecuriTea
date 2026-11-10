#pragma once
// SecuriTea board configuration: Heltec WiFi LoRa 32 V3 (ESP32-S3FN8).
//
// Single source of truth for every pin and bus assignment. The README pin table and the
// docs/ wiring diagram are generated from this file, so change pins here and nowhere else.
//
// Pins defined by the Heltec variant (pins_arduino.h) and used as-is:
//   SDA_OLED 17, SCL_OLED 18, RST_OLED 21, Vext 36 (drive LOW to power the OLED and header).
// ESP32-S3 strapping pins: 0, 3, 45, 46. GPIO3 is the joystick X axis; it is only sampled
// during reset, so a centred joystick is harmless. 45 and 46 are deliberately unused.

#include <stdint.h>

namespace board
{
// Joystick: analog X/Y on ADC1, button to GND with the internal pull-up.
constexpr uint8_t kJoyXPin = 3;
constexpr uint8_t kJoyYPin = 4;
constexpr uint8_t kJoyButtonPin = 5;

// Infrared receiver module and IR LED driver.
constexpr uint8_t kIrRecvPin = 6;
constexpr uint8_t kIrLedPin = 7;

// External SPI bus (FSPI = SPI2, routed through the GPIO matrix). Shared by the SD card and,
// later, the PN532 NFC board. Each device has its own chip select; the bus has one mutex.
constexpr uint8_t kExtSpiSckPin = 33;
constexpr uint8_t kExtSpiMisoPin = 34;
constexpr uint8_t kExtSpiMosiPin = 47;
constexpr uint8_t kSdCsPin = 48;
constexpr uint32_t kSdSpiHz = 4000000;

// PN532 NFC (reserved, not wired yet). On the red V3 board set DIP switches SEL0 off, SEL1 on
// for SPI. GPIO 38-40 are free on the V3 header; 39-42 double as JTAG, which this project
// does not use. Confirm against the Heltec V3 pinout diagram before wiring.
constexpr uint8_t kNfcCsPin = 38;
constexpr uint8_t kNfcIrqPin = 39;
constexpr uint8_t kNfcRstPin = 40;

// Onboard SX1262 LoRa / Sub-GHz radio (HSPI = SPI3). Fixed by the board layout and private
// to the radio driver; listed so nobody reuses these pins on the header.
constexpr uint8_t kRadioNssPin = 8;
constexpr uint8_t kRadioSckPin = 9;
constexpr uint8_t kRadioMosiPin = 10;
constexpr uint8_t kRadioMisoPin = 11;
constexpr uint8_t kRadioRstPin = 12;
constexpr uint8_t kRadioBusyPin = 13;
constexpr uint8_t kRadioDio1Pin = 14;

// Onboard SSD1306 OLED, 128x64, I2C.
constexpr uint8_t kOledAddress = 0x3c;
} // namespace board
