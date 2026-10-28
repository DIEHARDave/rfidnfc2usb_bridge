#include "emulator.h"

#include <ArduinoJson.h>

#include "version.h"

#if defined(ARDUINO_ARCH_SAMD)
#define BRIDGE_BOARD "samd21"
#elif defined(PICO_RP2350)
#define BRIDGE_BOARD "rp2350"
#elif defined(ARDUINO_ARCH_RP2040)
#define BRIDGE_BOARD "rp2040"
#else
#define BRIDGE_BOARD CONFIG_IDF_TARGET
#endif

namespace {

// PN532 commands (UM0701-02, section 7)
constexpr uint8_t DIAGNOSE = 0x00;
constexpr uint8_t GET_FIRMWARE_VERSION = 0x02;
constexpr uint8_t GET_GENERAL_STATUS = 0x04;
constexpr uint8_t READ_REGISTER = 0x06;
constexpr uint8_t WRITE_REGISTER = 0x08;
constexpr uint8_t READ_GPIO = 0x0C;
constexpr uint8_t WRITE_GPIO = 0x0E;
constexpr uint8_t SET_SERIAL_BAUD_RATE = 0x10;
constexpr uint8_t SET_PARAMETERS = 0x12;
constexpr uint8_t SAM_CONFIGURATION = 0x14;
constexpr uint8_t POWER_DOWN = 0x16;
constexpr uint8_t RF_CONFIGURATION = 0x32;
constexpr uint8_t IN_DATA_EXCHANGE = 0x40;
constexpr uint8_t IN_COMMUNICATE_THRU = 0x42;
constexpr uint8_t IN_DESELECT = 0x44;
constexpr uint8_t IN_LIST_PASSIVE_TARGET = 0x4A;
constexpr uint8_t IN_PSL = 0x4E;
constexpr uint8_t IN_RELEASE = 0x52;
constexpr uint8_t IN_SELECT = 0x54;
constexpr uint8_t IN_AUTO_POLL = 0x60;

// The bridge's own commands (docs/protocol.md)
constexpr uint8_t BRIDGE_INFO = 0xF0;
constexpr uint8_t ISO15693_INVENTORY = 0xF2;
constexpr uint8_t ISO15693_EXCHANGE = 0xF4;
constexpr uint8_t GET_CONFIG = 0xF6;
constexpr uint8_t SET_CONFIG = 0xF8;
constexpr uint8_t RESTART = 0xFA;

// Status bytes (UM0701-02, table 15)
constexpr uint8_t ST_OK = 0x00;
constexpr uint8_t ST_TIMEOUT = 0x01;
constexpr uint8_t ST_MIFARE_AUTH = 0x14;
constexpr uint8_t ST_CONTEXT = 0x27;    // command not acceptable in this state

// CIU registers that change how InCommunicateThru sends a frame
constexpr uint16_t CIU_TX_MODE = 0x6302;
constexpr uint16_t CIU_RX_MODE = 0x6303;
constexpr uint16_t CIU_BIT_FRAMING = 0x633D;

constexpr uint8_t TFI_HOST = 0xD4;
constexpr uint8_t TFI_PN532 = 0xD5;
constexpr size_t MAX_DATA = 262;         // largest response data
// A frame that stops arriving half way is dropped after this long.
constexpr uint32_t STALE_FRAME_MS = 100;
constexpr uint32_t RAW_TIMEOUT_MS = 50;

const uint8_t ACK[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
const uint8_t SYNTAX_ERROR[] = {0x00, 0x00, 0xFF, 0x01, 0xFF, 0x7F, 0x81, 0x00};
// What a PN532 v1.6 reports: IC, version, revision, supported (A, B, ISO 18092).
const uint8_t FIRMWARE[] = {0x32, 0x01, 0x06, 0x07};
const uint32_t BAUD_RATES[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1288000};

const uint8_t MAGIC[] = {'R', 'F', 'I', 'D', 'N', 'F', 'C', '2', 'U', 'S', 'B'};
constexpr uint8_t PROTOCOL_VERSION = 1;

// The UID as the host may give it in InListPassiveTarget's InitiatorData:
// with the cascade tags (0x88) of a 7 or 10 byte UID, as libnfc sends it.
size_t cascadedUid(const NfcTarget &t, uint8_t *out) {
    if (t.uidLen == 7) {
        out[0] = 0x88;
        memcpy(out + 1, t.uid, 7);
        return 8;
    }
    if (t.uidLen == 10) {
        out[0] = 0x88;
        memcpy(out + 1, t.uid, 3);
        out[4] = 0x88;
        memcpy(out + 5, t.uid + 3, 7);
        return 12;
    }
    memcpy(out, t.uid, t.uidLen);
    return t.uidLen;
}

bool uidMatches(const NfcTarget &t, const uint8_t *init, size_t len) {
    if (len == 0) return true;
    uint8_t cascaded[12];
    size_t n = cascadedUid(t, cascaded);
    return (len == t.uidLen && memcmp(init, t.uid, len) == 0) || (len == n && memcmp(init, cascaded, n) == 0);
}

// InListPassiveTarget's TargetData for one 106 kbps type A card.
size_t targetData(const NfcTarget &t, uint8_t *out) {
    size_t pos = 0;
    out[pos++] = 1;                 // Tg
    out[pos++] = t.atqa[0];
    out[pos++] = t.atqa[1];
    out[pos++] = t.sak;
    out[pos++] = t.uidLen;
    memcpy(out + pos, t.uid, t.uidLen);
    pos += t.uidLen;
    memcpy(out + pos, t.ats, t.atsLen);
    return pos + t.atsLen;
}

bool isMifareAck(int n, uint8_t bits, const uint8_t *resp) {
    return n == 1 && bits == 4 && (resp[0] & 0x0F) == 0x0A;
}

}  // namespace

// ---- Frames ---------------------------------------------------------------

Emulator::Frame Emulator::nextFrame(uint8_t *&body, size_t &len) {
    for (size_t start = 0; start + 3 <= rxLen_; start++) {
        if (rx_[start] != 0x00 || rx_[start + 1] != 0x00 || rx_[start + 2] != 0xFF) continue;
        if (start) {                    // wakeup run, postambles, noise
            consume(start);
            start = 0;
        }
        if (rxLen_ < 6) return Frame::None;
        if (rx_[3] == 0x00 && rx_[4] == 0xFF) {
            consume(6);
            return Frame::Ack;
        }
        if (rx_[3] == 0xFF && rx_[4] == 0x00) {
            consume(6);
            return Frame::Nack;
        }
        size_t length, header;
        if (rx_[3] == 0xFF && rx_[4] == 0xFF) {     // extended frame
            if (rxLen_ < 8) return Frame::None;
            if ((uint8_t)(rx_[5] + rx_[6] + rx_[7]) != 0) {
                consume(1);
                start = (size_t)-1;
                continue;
            }
            length = (rx_[5] << 8) | rx_[6];
            header = 8;
        } else {
            if ((uint8_t)(rx_[3] + rx_[4]) != 0) {  // not a frame start
                consume(1);
                start = (size_t)-1;
                continue;
            }
            length = rx_[3];
            header = 5;
        }
        if (length < 2 || header + length + 1 > sizeof(rx_)) {
            consume(1);
            start = (size_t)-1;
            continue;
        }
        if (rxLen_ < header + length + 1) return Frame::None;
        uint8_t sum = 0;
        for (size_t i = 0; i <= length; i++) sum += rx_[header + i];
        if (sum != 0 || rx_[header] != TFI_HOST) {
            // A PN532 ignores a frame with a bad checksum; the host times
            // out and sends it again.
            consume(header + length + 1);
            return Frame::None;
        }
        body = rx_ + header;
        len = length;
        return Frame::Data;
    }
    // Nothing that starts a frame: keep only a possible preamble start.
    if (rxLen_ > 2) consume(rxLen_ - 2);
    return Frame::None;
}

void Emulator::consume(size_t count) {
    count = min(count, rxLen_);
    memmove(rx_, rx_ + count, rxLen_ - count);
    rxLen_ -= count;
}

void Emulator::sendAck() {
    host_.write(ACK, sizeof(ACK));
}

void Emulator::sendFrame(const uint8_t *body, size_t len) {
    size_t pos = 0;
    last_[pos++] = 0x00;
    last_[pos++] = 0x00;
    last_[pos++] = 0xFF;
    if (len > 255) {
        last_[pos++] = 0xFF;
        last_[pos++] = 0xFF;
        last_[pos++] = len >> 8;
        last_[pos++] = len & 0xFF;
        last_[pos++] = (uint8_t)(-(int)((len >> 8) + (len & 0xFF)));
    } else {
        last_[pos++] = len;
        last_[pos++] = (uint8_t)(-(int)len);
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        last_[pos++] = body[i];
        sum += body[i];
    }
    last_[pos++] = (uint8_t)(-(int)sum);
    last_[pos++] = 0x00;
    lastLen_ = pos;
    host_.write(last_, lastLen_);
    host_.flush();
}

void Emulator::sendSyntaxError() {
    memcpy(last_, SYNTAX_ERROR, sizeof(SYNTAX_ERROR));
    lastLen_ = sizeof(SYNTAX_ERROR);
    host_.write(last_, lastLen_);
    host_.flush();
}

bool Emulator::waitAborted(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        if (aborted()) return true;
        delay(1);
    }
    return aborted();
}

void Emulator::poll() {
    int available = host_.available();
    if (available > 0) {
        size_t room = sizeof(rx_) - rxLen_;
        size_t n = host_.readBytes(rx_ + rxLen_, min((size_t)available, room));
        rxLen_ += n;
        lastRx_ = millis();
    } else if (rxLen_ && millis() - lastRx_ > STALE_FRAME_MS) {
        rxLen_ = 0;
    }
    if (pendingBaud_ && millis() - baudAt_ > 50) {
        // The host did not ACK; switch anyway, as it will.
        setBaud_(pendingBaud_);
        pendingBaud_ = 0;
    }

    while (true) {
        uint8_t *body;
        size_t len;
        Frame frame = nextFrame(body, len);
        if (frame == Frame::None) return;
        if (frame == Frame::Ack) {
            // Ends a command (already finished here) or confirms a baud change.
            if (pendingBaud_) {
                host_.flush();
                setBaud_(pendingBaud_);
                pendingBaud_ = 0;
            }
            continue;
        }
        if (frame == Frame::Nack) {
            if (lastLen_) host_.write(last_, lastLen_);
            continue;
        }
        uint8_t cmd = body[1];
        uint8_t params[300];
        size_t paramLen = len - 2;
        memcpy(params, body + 2, paramLen);
        consume((body - rx_) + len + 1);
        sendAck();

        uint8_t resp[2 + MAX_DATA + 8];
        int n = handle(cmd, params, paramLen, resp + 2);
        if (n == -2) continue;                    // aborted: no answer
        if (n < 0) {
            sendSyntaxError();
            continue;
        }
        resp[0] = TFI_PN532;
        resp[1] = cmd + 1;
        sendFrame(resp, n + 2);
        if (restart_) {
            delay(100);
#if defined(ARDUINO_ARCH_SAMD)
            NVIC_SystemReset();
#elif defined(ARDUINO_ARCH_RP2040)
            rp2040.reboot();
#else
            ESP.restart();
#endif
        }
    }
}

// ---- Commands -------------------------------------------------------------

int Emulator::passThrough(uint8_t cmd, const uint8_t *p, size_t len, uint8_t *out, uint32_t timeoutMs) {
    return backend_.pn532()->command(cmd, p, len, out, MAX_DATA, timeoutMs);
}

int Emulator::handle(uint8_t cmd, const uint8_t *p, size_t len, uint8_t *out) {
    Pn532 *pn532 = backend_.pn532();
    NfcChip *chip = backend_.chip();

    switch (cmd) {
    // Bridge commands, the same for every chip.
    case BRIDGE_INFO:
        return bridgeInfo(out);
    case ISO15693_INVENTORY:
        return iso15693Inventory(out);
    case ISO15693_EXCHANGE:
        return iso15693Exchange(p, len, out);
    case GET_CONFIG:
        return getConfig(out);
    case SET_CONFIG:
        return setConfig(p, len, out);
    case RESTART:
        restart_ = true;
        return 0;

    // Handled here even with a PN532 behind the bridge.
    case IN_LIST_PASSIVE_TARGET:
        return listPassiveTarget(p, len, out);
    case IN_AUTO_POLL:
        return autoPoll(p, len, out);
    case RF_CONFIGURATION:
        return rfConfiguration(p, len, out);
    case SET_SERIAL_BAUD_RATE:
        // The PN532 behind the bridge keeps its own link; only the host's
        // UART changes. Over USB the baud rate means nothing.
        if (len < 1 || p[0] >= sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0])) return -1;
        if (setBaud_) {
            pendingBaud_ = BAUD_RATES[p[0]];
            baudAt_ = millis();
        }
        return 0;
    case POWER_DOWN:
        // The bridge stays awake for the host; the card is let go.
        releaseTarget();
        out[0] = ST_OK;
        return 1;
    default:
        break;
    }

    if (pn532) {
        switch (cmd) {
        case IN_DATA_EXCHANGE:
        case IN_COMMUNICATE_THRU:
            return passThrough(cmd, p, len, out, 2000);
        case IN_RELEASE:
            selected_ = false;
            return passThrough(cmd, p, len, out);
        case SET_PARAMETERS:
            if (len >= 1) parameters_ = p[0];
            return passThrough(cmd, p, len, out);
        default:
            return passThrough(cmd, p, len, out);
        }
    }

    // RC522 / PN5180, or no chip at all.
    switch (cmd) {
    case DIAGNOSE:
        if (len < 1) return -1;
        if (p[0] == 0x00) {             // communication line test: echo
            memcpy(out, p, len);
            return len;
        }
        out[0] = ST_OK;                 // the self tests pass
        return 1;
    case GET_FIRMWARE_VERSION:
        memcpy(out, FIRMWARE, sizeof(FIRMWARE));
        return sizeof(FIRMWARE);
    case GET_GENERAL_STATUS:
        return generalStatus(out);
    case READ_REGISTER:
        if (len < 2 || len % 2) return -1;
        for (size_t i = 0; i < len / 2; i++) out[i] = readRegister((p[2 * i] << 8) | p[2 * i + 1]);
        return len / 2;
    case WRITE_REGISTER:
        if (len < 3 || len % 3) return -1;
        for (size_t i = 0; i < len; i += 3) writeRegister((p[i] << 8) | p[i + 1], p[i + 2]);
        return 0;
    case READ_GPIO:
        out[0] = 0xFF;                  // P3
        out[1] = 0xFF;                  // P7
        out[2] = 0x00;                  // I0I1
        return 3;
    case WRITE_GPIO:
    case SAM_CONFIGURATION:
        return 0;
    case SET_PARAMETERS:
        if (len < 1) return -1;
        parameters_ = p[0];
        if (chip) chip->setAutoIsoDep(parameters_ & 0x10);
        return 0;
    case IN_DATA_EXCHANGE:
        return dataExchange(p, len, out);
    case IN_COMMUNICATE_THRU:
        return communicateThru(p, len, out);
    case IN_DESELECT:
        if (chip && selected_) chip->release();
        selected_ = false;
        out[0] = ST_OK;
        return 1;
    case IN_RELEASE:
        releaseTarget();
        known_ = false;
        out[0] = ST_OK;
        return 1;
    case IN_SELECT: {
        NfcTarget again;
        if (!chip || !known_ || len < 1 || !chip->listTarget(again) || !again.sameCard(target_)) {
            out[0] = ST_TIMEOUT;
            return 1;
        }
        target_ = again;
        selected_ = true;
        out[0] = ST_OK;
        return 1;
    }
    case IN_PSL:
        out[0] = ST_OK;                 // 106 kbps stays
        return 1;
    default:
        // Target mode, ISO 18092 (DEP), FeliCa and type B need a PN532.
        return -1;
    }
}

int Emulator::listOnce(uint8_t maxTg, uint8_t brTy, const uint8_t *init, size_t initLen, uint8_t *out) {
    out[0] = 0;
    if (Pn532 *pn532 = backend_.pn532()) {
        uint8_t params[2 + 64];
        params[0] = maxTg;
        params[1] = brTy;
        initLen = min(initLen, sizeof(params) - 2);
        memcpy(params + 2, init, initLen);
        // The chip itself retries only briefly (see rfConfiguration).
        int n = pn532->command(IN_LIST_PASSIVE_TARGET, params, initLen + 2, out, MAX_DATA, 1000);
        if (n < 1) {
            out[0] = 0;
            return 1;
        }
        selected_ = out[0] > 0;
        return n;
    }
    NfcChip *chip = backend_.chip();
    if (!chip || brTy != 0x00) return 1;        // only 106 kbps type A
    NfcTarget t;
    if (!chip->listTarget(t)) return 1;
    if (!uidMatches(t, init, initLen)) {
        chip->release();
        return 1;
    }
    vicinity_ = false;
    target_ = t;
    selected_ = known_ = true;
    out[0] = 1;
    return 1 + targetData(t, out + 1);
}

int Emulator::listPassiveTarget(const uint8_t *p, size_t len, uint8_t *out) {
    if (len < 2 || p[0] < 1 || p[0] > 2) return -1;
    releaseTarget();
    // MxRtyPassiveActivation 0xFF: until a card comes or the host aborts.
    for (uint32_t attempt = 0; mxRtyPassive_ == 0xFF || attempt <= mxRtyPassive_; attempt++) {
        int n = listOnce(p[0], p[1], p + 2, len - 2, out);
        if (out[0] > 0) return n;
        if (waitAborted(mxRtyPassive_ == 0xFF ? 10 : 1)) return -2;
    }
    out[0] = 0;
    return 1;
}

int Emulator::autoPoll(const uint8_t *p, size_t len, uint8_t *out) {
    if (len < 3 || len > 17) return -1;
    uint8_t pollNr = p[0], period = p[1];
    const uint8_t *types = p + 2;
    size_t typeCount = len - 2;
    releaseTarget();
    for (uint32_t round = 0; pollNr == 0xFF || round < pollNr; round++) {
        for (size_t i = 0; i < typeCount; i++) {
            uint8_t type = types[i], brTy;
            uint8_t init[5] = {};
            size_t initLen = 0;
            switch (type & 0x0F) {
            case 0x00: brTy = 0x00; break;                      // 106 kbps type A
            case 0x01:                                          // FeliCa 212 kbps
            case 0x02:                                          // FeliCa 424 kbps
                brTy = type & 0x0F;
                init[0] = 0x00, init[1] = 0xFF, init[2] = 0xFF, init[3] = 0x01, init[4] = 0x00;
                initLen = 5;
                break;
            case 0x03: brTy = 0x03; initLen = 1; break;         // type B, AFI 0
            case 0x04: brTy = 0x04; break;                      // Jewel
            default: continue;                                  // DEP: needs a PN532
            }
            if (type == 0x40 || type == 0x41 || type == 0x42 || type == 0x80 || type == 0x81 || type == 0x82)
                continue;
            uint8_t resp[MAX_DATA];
            int n = listOnce(1, brTy, init, initLen, resp);
            if (resp[0] == 0) continue;
            // 0x20: ISO 14443-4 type A only; 0x10: MIFARE (no ISO-DEP).
            uint8_t sak = n > 4 ? resp[4] : 0;
            if ((type == 0x20 && !(sak & 0x20)) || (type == 0x10 && (sak & 0x20))) {
                releaseTarget();
                continue;
            }
            out[0] = 1;
            out[1] = type;
            out[2] = n - 1;
            memcpy(out + 3, resp + 1, n - 1);
            return n + 2;
        }
        if (waitAborted(period * 150)) return -2;
    }
    out[0] = 0;
    return 1;
}

int Emulator::dataExchange(const uint8_t *p, size_t len, uint8_t *out) {
    NfcChip *chip = backend_.chip();
    if (len < 2 || (p[0] & 0x0F) != 1 || !chip || !selected_) {
        out[0] = ST_CONTEXT;
        return 1;
    }
    const uint8_t *data = p + 1;
    size_t n = len - 1;
    if (target_.atsLen) {                         // ISO-DEP: an APDU
        int got = chip->exchange(data, n, out + 1, MAX_DATA - 1, 2000);
        if (got < 0) {
            out[0] = ST_TIMEOUT;
            return 1;
        }
        out[0] = ST_OK;
        return got + 1;
    }
    uint8_t op = data[0];
    if ((op == 0x60 || op == 0x61) && n == 12) {  // MIFARE Classic authentication
        out[0] = chip->exchange(data, n, out + 1, 0) >= 0 ? ST_OK : ST_MIFARE_AUTH;
        return 1;
    }
    // Commands whose answer is a 4 bit ACK, which exchange() takes for a NAK.
    if ((op == 0xA0 && n == 18) || (op == 0xA2 && n == 6) || ((op >= 0xC0 && op <= 0xC2) && n == 6) ||
        (op == 0xB0 && n == 2)) {
        out[0] = mifareWrite(data, n) ? ST_OK : ST_TIMEOUT;
        return 1;
    }
    int got = chip->exchange(data, n, out + 1, MAX_DATA - 1);
    if (got < 0) {
        out[0] = ST_TIMEOUT;
        return 1;
    }
    out[0] = ST_OK;
    return got + 1;
}

// MIFARE writes, as the PN532 runs them inside InDataExchange: WRITE (A0,
// two steps) and COMPATIBILITY WRITE, Ultralight/NTAG WRITE (A2), and the
// value block commands (C0-C2: two steps, the second unanswered) and
// TRANSFER (B0). 1 on success, 0 otherwise.
int Emulator::mifareWrite(const uint8_t *data, size_t len) {
    NfcChip *chip = backend_.chip();
    uint8_t resp[4], bits = 0;
    uint8_t op = data[0];
    if (op == 0xA2 || op == 0xB0) {
        int n = chip->transceiveRaw(data, len, resp, sizeof(resp), true, 0, &bits, RAW_TIMEOUT_MS);
        return isMifareAck(n, bits, resp);
    }
    int n = chip->transceiveRaw(data, 2, resp, sizeof(resp), true, 0, &bits, RAW_TIMEOUT_MS);
    if (!isMifareAck(n, bits, resp)) return 0;
    if (op == 0xA0) {
        n = chip->transceiveRaw(data + 2, 16, resp, sizeof(resp), true, 0, &bits, RAW_TIMEOUT_MS);
        return isMifareAck(n, bits, resp);
    }
    // Value commands: no answer to the operand means it was taken; a NAK
    // means it was not.
    n = chip->transceiveRaw(data + 2, 4, resp, sizeof(resp), true, 0, &bits, 5);
    return n < 0 || isMifareAck(n, bits, resp);
}

int Emulator::communicateThru(const uint8_t *p, size_t len, uint8_t *out) {
    NfcChip *chip = backend_.chip();
    if (!chip || len < 1) {
        out[0] = ST_TIMEOUT;
        return 1;
    }
    bool crc = readRegister(CIU_TX_MODE) & 0x80;
    uint8_t txBits = readRegister(CIU_BIT_FRAMING) & 0x07;
    uint8_t bits = 0;
    int n = chip->transceiveRaw(p, len, out + 1, MAX_DATA - 1, crc, txBits, &bits, RAW_TIMEOUT_MS);
    if (n < 0) {
        out[0] = ST_TIMEOUT;
        return 1;
    }
    // The valid bits of the last byte received, where the PN532 keeps them.
    writeRegister(0x633C, (readRegister(0x633C) & 0xF8) | bits);
    out[0] = ST_OK;
    return n + 1;
}

int Emulator::rfConfiguration(const uint8_t *p, size_t len, uint8_t *out) {
    if (len < 2) return -1;
    uint8_t item = p[0];
    if (item == 0x01 && !(p[1] & 0x01)) releaseTarget();     // RF field off
    if (item == 0x05 && len >= 4) mxRtyPassive_ = p[3];
    Pn532 *pn532 = backend_.pn532();
    if (!pn532) return 0;
    uint8_t params[16];
    len = min(len, sizeof(params));
    memcpy(params, p, len);
    // The bridge does the passive activation retries itself, so that it can
    // stop for the host; the chip tries briefly each time.
    if (item == 0x05 && len >= 4) params[3] = 0x01;
    return pn532->command(RF_CONFIGURATION, params, len, out, MAX_DATA, 1000);
}

int Emulator::generalStatus(uint8_t *out) {
    size_t pos = 0;
    out[pos++] = 0x00;                          // last error
    out[pos++] = backend_.chip() ? 0x01 : 0x00; // external field
    out[pos++] = selected_ ? 1 : 0;             // targets
    if (selected_) {
        out[pos++] = 1;                         // Tg
        out[pos++] = 0x00;                      // BrRx: 106 kbps
        out[pos++] = 0x00;                      // BrTx
        out[pos++] = 0x00;                      // modulation: ISO 14443A
    }
    out[pos++] = 0x00;                          // SAM status
    return pos;
}

int Emulator::bridgeInfo(uint8_t *out) {
    NfcChip *chip = backend_.chip();
    size_t pos = 0;
    memcpy(out, MAGIC, sizeof(MAGIC));
    pos += sizeof(MAGIC);
    out[pos++] = PROTOCOL_VERSION;
    uint8_t code = 0;
    if (chip) {
        String name = chip->chipName();
        code = name == "PN532" ? 1 : name == "RC522" ? 2 : name == "PN5180" ? 3 : 0;
    }
    out[pos++] = code;
    out[pos++] = chip && chip->hasIso15693() ? 0x01 : 0x00;
    String text = String("firmware=") + FIRMWARE_VERSION + ";board=" + BRIDGE_BOARD +
                  ";reader=" + backend_.describe() + ";setting=" + chipSettingName(config_.chip);
    size_t n = min((size_t)text.length(), MAX_DATA - pos);
    memcpy(out + pos, text.c_str(), n);
    return pos + n;
}

int Emulator::iso15693Inventory(uint8_t *out) {
    NfcChip *chip = backend_.chip();
    releaseTarget();
    NfcTarget t;
    if (!chip || !chip->hasIso15693() || !chip->inventory15693(t)) {
        out[0] = 0;
        return 1;
    }
    vicinity_ = true;
    out[0] = 1;
    memcpy(out + 1, t.uid, 8);
    return 9;
}

int Emulator::iso15693Exchange(const uint8_t *p, size_t len, uint8_t *out) {
    NfcChip *chip = backend_.chip();
    if (!chip || !vicinity_ || len < 2) {
        out[0] = ST_CONTEXT;
        return 1;
    }
    int n = chip->transceiveRaw(p, len, out + 1, MAX_DATA - 1, true, 0, nullptr, 60);
    if (n < 1) {
        out[0] = ST_TIMEOUT;
        return 1;
    }
    out[0] = ST_OK;
    return n + 1;
}

int Emulator::getConfig(uint8_t *out) {
    JsonDocument doc;
    configToJson(config_, doc.to<JsonObject>());
    return serializeJson(doc, (char *)out, MAX_DATA);
}

int Emulator::setConfig(const uint8_t *p, size_t len, uint8_t *out) {
    JsonDocument doc;
    String error;
    if (deserializeJson(doc, (const char *)p, len) || !doc.is<JsonObject>()) {
        error = "settings must be a JSON object";
    } else if (doc["reset"] == true) {
        resetConfig();
    } else {
        Config next = config_;
        error = configFromJson(next, doc.as<JsonObjectConst>());
        if (!error.length()) saveConfig(next);
    }
    out[0] = !error.length() ? ST_OK : 0x01;
    // The new settings apply after a restart, which follows the answer.
    restart_ = !error.length();
    size_t n = min((size_t)error.length(), MAX_DATA - 1);
    memcpy(out + 1, error.c_str(), n);
    return n + 1;
}

void Emulator::releaseTarget() {
    if (Pn532 *pn532 = backend_.pn532()) {
        if (selected_) {
            const uint8_t all[] = {0x00};
            uint8_t resp[4];
            pn532->command(IN_RELEASE, all, sizeof(all), resp, sizeof(resp), 300);
        }
    } else if (NfcChip *chip = backend_.chip()) {
        if (selected_) chip->release();
    }
    selected_ = false;
}

uint8_t Emulator::readRegister(uint16_t address) const {
    for (size_t i = 0; i < regCount_; i++)
        if (regs_[i].address == address) return regs_[i].value;
    // TxCRCEn / RxCRCEn: frames carry CRC_A unless the host turns it off.
    if (address == CIU_TX_MODE || address == CIU_RX_MODE) return 0x80;
    return 0x00;
}

void Emulator::writeRegister(uint16_t address, uint8_t value) {
    for (size_t i = 0; i < regCount_; i++) {
        if (regs_[i].address == address) {
            regs_[i].value = value;
            return;
        }
    }
    if (regCount_ < sizeof(regs_) / sizeof(regs_[0])) regs_[regCount_++] = {address, value};
}
