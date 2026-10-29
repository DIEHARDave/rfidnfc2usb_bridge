// NXP PN5180 over SPI (plus its BUSY line): ISO 14443A like the PN532
// (NTAG / Ultralight, MIFARE Classic, ISO-DEP through IsoDep) and ISO 15693
// (NFC-V: ICODE SLIX/SLIX2, used by OpenPrintTag), which the PN532 and
// RC522 cannot read. Command layer as in FF_NFC's readers/pn5180.py.
//
// The common PN5180 boards want 5 V for the RF stage and 3.3 V for logic.
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "iso_dep.h"
#include "nfc_chip.h"

class Pn5180 : public NfcChip, private FrameLink {
public:
    Pn5180() : isoDep_(*this) {}
    // rst: -1 when RST is not wired (the board pulls it up).
    void attach(SPIClass &spi, int sck, int miso, int mosi, int nss, int busy, int rst);
    bool init() override;
    void end() override;
    bool ready() const override { return ok_; }
    const char *chipName() const override { return "PN5180"; }
    const char *busName() const override { return "SPI"; }
    String firmware() const override { return firmware_; }

    bool listTarget(NfcTarget &target) override;
    int exchange(const uint8_t *data, size_t len, uint8_t *out, size_t expect,
                 uint32_t timeoutMs = 1000) override;
    void release() override;
    int failures() const override { return failures_; }
    void setAutoIsoDep(bool on) override { autoIsoDep_ = on; }
    int transceiveRaw(const uint8_t *data, size_t len, uint8_t *out, size_t max, bool crc,
                      uint8_t txBits, uint8_t *rxBits, uint32_t timeoutMs) override;

    bool hasIso15693() const override { return true; }
    bool inventory15693(NfcTarget &target) override;
    int exchange15693(const uint8_t *request, size_t len, uint8_t *out, size_t max) override;

private:
    enum class Rf : uint8_t { Off, Iso14443A, Iso15693 };

    // One host interface command: send it, then read recvLen answer bytes.
    bool command(const uint8_t *send, size_t len, uint8_t *recv = nullptr, size_t recvLen = 0);
    bool waitBusy(int level, uint32_t timeoutMs);
    bool writeRegister(uint8_t reg, uint32_t value);
    bool orRegister(uint8_t reg, uint32_t mask);
    bool andRegister(uint8_t reg, uint32_t mask);
    bool readRegister(uint8_t reg, uint32_t &value);
    bool clearIrq() { return writeRegister(0x03, 0x000FFFFF); }
    // Load the protocol's RF configuration and switch the field on.
    bool rfMode(Rf mode);
    void rfOff();
    void setCrc(bool on);
    // Send a frame; the answer (its CRC removed when CRC is on), or -1.
    int transceive(const uint8_t *data, size_t len, uint8_t *out, size_t max, uint32_t timeoutMs,
                   uint8_t txBits = 0, uint8_t *rxBits = nullptr);
    int transceiveFrame(const uint8_t *data, size_t len, uint8_t *out, size_t max,
                        uint32_t timeoutMs) override;
    bool select(NfcTarget &target);

    SPIClass *spi_ = nullptr;
    int nss_ = -1, busy_ = -1, rst_ = -1;
    bool ok_ = false;
    int failures_ = 0;
    String firmware_;
    Rf rf_ = Rf::Off;
    int crc_ = -1;              // CRC setting last written; -1: unknown
    IsoDep isoDep_;
    bool autoIsoDep_ = true;
};
