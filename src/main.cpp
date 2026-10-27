// esp32_rfidnfc2usb_bridge: a PN532, RC522 or PN5180 on an ESP32 that the
// host sees as a PN532 on a serial port. Boards with native USB (S2, S3,
// C3, C6) appear as a USB CDC-ACM device, which Windows 10+, Linux and
// macOS drive without extra drivers; classic ESP32 boards use their
// USB-UART chip at 115200 baud, as a PN532 HSU module would.
#include <Arduino.h>

#include "backend.h"
#include "config.h"
#include "emulator.h"

#if ARDUINO_USB_CDC_ON_BOOT || defined(ARDUINO_ARCH_SAMD)
#define HOST_IS_UART 0
#else
#define HOST_IS_UART 1
#endif

namespace {

constexpr uint32_t HOST_BAUD = 115200;      // the PN532 HSU default

Config config;
Backend backend;
std::function<void(uint32_t)> setHostBaud =
#if HOST_IS_UART
    [](uint32_t baud) { Serial.updateBaudRate(baud); };
#else
    nullptr;
#endif
Emulator emulator(Serial, backend, config, setHostBaud);

// The LED: off while the reader waits, lit while the host has a card
// selected, blinking slowly when no reader chip answers.
void updateLed() {
    if (config.led < 0) return;
    bool on;
    if (!backend.chip()) on = (millis() / 500) % 2;
    else on = emulator.cardSelected();
    digitalWrite(config.led, on != config.ledActiveLow ? HIGH : LOW);
}

}  // namespace

void setup() {
#if HOST_IS_UART
    Serial.setRxBufferSize(1024);
#endif
#if defined(ARDUINO_ARCH_SAMD)
    // The SAMD core drops writes while DTR is low unless begun this way;
    // bridgectl and some PN532 hosts open the port with DTR off.
    Serial.beginWithoutDTR(HOST_BAUD);
#else
    Serial.begin(HOST_BAUD);
#endif
    loadConfig(config);
    if (config.led >= 0) {
        pinMode(config.led, OUTPUT);
        digitalWrite(config.led, config.ledActiveLow ? HIGH : LOW);
    }
    backend.begin(config);
}

void loop() {
    emulator.poll();
    if (emulator.idle()) backend.service();
    updateLed();
    delay(1);
}
