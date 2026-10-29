// What the PN532 emulator needs from a reader chip: find one ISO 14443A
// card (or, on the PN5180, an ISO 15693 one) and exchange frames with it.
// Pn532, Rc522 and Pn5180 implement it.
#pragma once

#include <Arduino.h>

struct NfcTarget {
    uint8_t uid[10] = {};
    uint8_t uidLen = 0;
    uint8_t atqa[2] = {};
    uint8_t sak = 0;
    // The card's answer to RATS (starting with its length byte TL) when the
    // chip activated ISO-DEP itself; atsLen 0 otherwise.
    uint8_t ats[32] = {};
    uint8_t atsLen = 0;
    // ISO 15693 (NFC-V): uid is the 8 byte UID, most significant byte (E0)
    // first, as FF_NFC reports it; atqa and sak stay zero.
    bool iso15693 = false;
    bool sameCard(const NfcTarget &other) const {
        return uidLen == other.uidLen && memcmp(uid, other.uid, uidLen) == 0;
    }
};

class NfcChip {
public:
    virtual ~NfcChip() = default;
    // Reset the chip and check that it answers.
    virtual bool init() = 0;
    virtual void end() = 0;
    virtual bool ready() const = 0;
    virtual const char *chipName() const = 0;
    virtual const char *busName() const = 0;
    virtual String firmware() const = 0;

    // Select one card (a halted one too).
    virtual bool listTarget(NfcTarget &target) = 0;
    // A command to the selected card, as the PN532's InDataExchange takes
    // it: MIFARE commands without CRC, MIFARE authentication as
    // {0x60|0x61, block, key[6], uid[4]}, and for ISO-DEP cards the APDU
    // alone. The card's answer, or -1 on any error.
    virtual int exchange(const uint8_t *data, size_t len, uint8_t *out, size_t expect,
                         uint32_t timeoutMs = 1000) = 0;
    virtual void release() = 0;
    // Failed commands in a row; the caller re-inits the chip after a few.
    virtual int failures() const = 0;

    // ISO-DEP activation (RATS) during listTarget, on by default; the host
    // can switch it off to send RATS itself (PN532 SetParameters).
    virtual void setAutoIsoDep(bool on) { (void)on; }

    // One raw frame to the card, as the PN532's InCommunicateThru sends it:
    // CRC_A added and checked when crc is set, txBits bits of the last byte
    // sent (0: all 8). The answer (CRC removed), with the valid bits of its
    // last byte in rxBits (0: all 8; a MIFARE ACK/NAK has 4), or -1 when no
    // valid answer came. Not available on the PN532, which the emulator
    // passes commands through to instead.
    virtual int transceiveRaw(const uint8_t *data, size_t len, uint8_t *out, size_t max, bool crc,
                              uint8_t txBits, uint8_t *rxBits, uint32_t timeoutMs) {
        (void)data; (void)len; (void)out; (void)max; (void)crc; (void)txBits; (void)rxBits;
        (void)timeoutMs;
        return -1;
    }

    // ISO 15693, on chips that have it.
    virtual bool hasIso15693() const { return false; }
    // Inventory with one slot: the card in the field, or false.
    virtual bool inventory15693(NfcTarget &) { return false; }
    // A request to the card found last; the answer after the flags byte, or
    // -1 when the card reported an error or did not answer.
    virtual int exchange15693(const uint8_t *, size_t, uint8_t *, size_t) { return -1; }
};
