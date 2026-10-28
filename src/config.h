// Settings: which reader chip, on which pins. Kept as JSON in NVS and
// changed over USB with the bridge's own commands (tools/bridgectl.py).
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum class ChipSetting : uint8_t {
    Auto = 0,       // probe PN532 (I2C, then UART), PN5180, RC522 at boot
    Pn532I2C = 1,
    Pn532Uart = 2,
    Rc522 = 3,
    Pn5180 = 4,
};

struct Config {
    ChipSetting chip = ChipSetting::Auto;
    int sda = -1, scl = -1;                         // PN532 I2C
    int rx = -1, tx = -1;                           // PN532 UART (ESP32 side)
    int sck = -1, miso = -1, mosi = -1, ss = -1;    // RC522 / PN5180 SPI
    int rst = -1;                                   // RC522 / PN5180 RST, -1: not wired
    int busy = -1;                                  // PN5180 BUSY
    int led = -1;                                   // status LED, -1: none
    bool ledActiveLow = false;
};

#if defined(ARDUINO_ARCH_RP2040)
// RP2040 / RP2350: each GPIO reaches one I2C, UART and SPI block, so the
// pins pick the block (0 or 1). SDA, UART TX and SPI RX sit on pins 0 mod 4
// (or 2 for I2C1), and the blocks alternate every 2, 8 and 8 pins.
inline int rpI2cBlock(int pin) { return (pin / 2) % 2; }
inline int rpUartBlock(int pin) { return ((pin + 4) / 8) % 2; }
inline int rpSpiBlock(int pin) { return (pin / 8) % 2; }
#endif

const char *chipSettingName(ChipSetting chip);

void loadConfig(Config &config);
void saveConfig(const Config &config);
void resetConfig();

void configToJson(const Config &config, JsonObject out);
// Applies the keys present in in. Returns an error message, or "" when the
// result is valid.
String configFromJson(Config &config, JsonObjectConst in);
