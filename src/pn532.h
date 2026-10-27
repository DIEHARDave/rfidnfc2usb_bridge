// NXP PN532 over I2C or HSU (UART). ISO 14443A only: NTAG / Ultralight,
// MIFARE Classic and ISO-DEP (Type 4). Port of zmod_nfc's pn532.py; the
// bridge passes the host's PN532 commands through to it.
//
// Board switches: I2C is SEL0=1 SEL1=0 (red/blue "PN532 NFC V3" boards:
// switch 1 ON, 2 OFF); HSU is both OFF.
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Wire.h>

#include "nfc_chip.h"

enum class Pn532Bus : uint8_t { I2C = 0, UART = 1 };

class Pn532 : public NfcChip {
public:
    void attachI2C(TwoWire &wire, int sda, int scl);
    void attachUart(HardwareSerial &serial, int rx, int tx, uint32_t baud = 115200);
    // Wake the chip, read its firmware version and set short RF retries.
    bool init() override { return init(500); }
    // timeoutMs per command: short when probing for a chip that may be absent.
    bool init(uint32_t timeoutMs);
    // I2C: whether anything acknowledges the PN532's address (a quick check
    // before init). Always true on UART.
    bool present();
    // I2C: "SDA" or "SCL" when that line stays low with the bus idle (after
    // clocking out a device stuck mid-byte), else nullptr. Transfers on
    // such a bus each hang until they time out.
    const char *stuckLine();
    // I2C: false when SDA or SCL has no pull-up, i.e. nothing is wired.
    bool pulledUp();
    void end() override;
    bool ready() const override { return ok_; }
    const char *chipName() const override { return "PN532"; }
    String firmware() const override { return firmware_; }
    const char *busName() const override { return bus_ == Pn532Bus::I2C ? "I2C" : "UART"; }

    // Response data after the response code, or -1. expect is the largest
    // response wanted (I2C reads that many bytes in one transfer).
    int command(uint8_t cmd, const uint8_t *params, size_t len, uint8_t *out, size_t expect,
                uint32_t timeoutMs = 1000, bool wake = false);

    bool listTarget(NfcTarget &target) override;
    // InDataExchange with the selected target: the card's answer (without
    // the status byte), or -1 when the PN532 or the card reported an error.
    int exchange(const uint8_t *data, size_t len, uint8_t *out, size_t expect,
                 uint32_t timeoutMs = 1000) override;
    void release() override;
    int failures() const override { return failures_; }

private:
    bool writeFrame(const uint8_t *data, size_t len, bool wake);
    // 0: ACK, >0: body length (TFI onwards), -1: timeout, -2: bad frame.
    int readFrame(uint8_t *body, size_t max, uint32_t timeoutMs, size_t i2cLen);
    bool i2cFill(size_t len, uint32_t deadline);
    void uartFill(uint32_t deadline);
    int parse(uint8_t *body, size_t max);

    Pn532Bus bus_ = Pn532Bus::I2C;
    TwoWire *wire_ = nullptr;
    int sda_ = -1, scl_ = -1;
    HardwareSerial *serial_ = nullptr;
    bool ok_ = false;
    int failures_ = 0;
    String firmware_;
    uint8_t rx_[300 + 16];
    size_t rxLen_ = 0;
};
