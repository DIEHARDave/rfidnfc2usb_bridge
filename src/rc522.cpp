// Register and command use follows the MFRC522 datasheet (rev. 3.9) and
// the ISO 14443-3/-4 framing the PN532 does on its own.
#include "rc522.h"

namespace {

constexpr uint32_t SPI_CLOCK = 4000000;

// Registers
constexpr uint8_t CommandReg = 0x01;
constexpr uint8_t ComIrqReg = 0x04;
constexpr uint8_t ErrorReg = 0x06;
constexpr uint8_t Status2Reg = 0x08;
constexpr uint8_t FIFODataReg = 0x09;
constexpr uint8_t FIFOLevelReg = 0x0A;
constexpr uint8_t ControlReg = 0x0C;
constexpr uint8_t BitFramingReg = 0x0D;
constexpr uint8_t ModeReg = 0x11;
constexpr uint8_t TxModeReg = 0x12;
constexpr uint8_t RxModeReg = 0x13;
constexpr uint8_t TxControlReg = 0x14;
constexpr uint8_t TxASKReg = 0x15;
constexpr uint8_t ModWidthReg = 0x24;
constexpr uint8_t RFCfgReg = 0x26;
constexpr uint8_t TModeReg = 0x2A;
constexpr uint8_t TPrescalerReg = 0x2B;
constexpr uint8_t TReloadRegH = 0x2C;
constexpr uint8_t TReloadRegL = 0x2D;
constexpr uint8_t VersionReg = 0x37;

// Commands
constexpr uint8_t CMD_IDLE = 0x00;
constexpr uint8_t CMD_TRANSCEIVE = 0x0C;
constexpr uint8_t CMD_MF_AUTHENT = 0x0E;
constexpr uint8_t CMD_SOFT_RESET = 0x0F;

constexpr uint8_t IRQ_TIMER = 0x01;
constexpr uint8_t IRQ_IDLE = 0x10;
constexpr uint8_t IRQ_RX = 0x20;
constexpr uint8_t ERR_COLLISION = 0x08;
// BufferOvfl, ParityErr, ProtocolErr
constexpr uint8_t ERR_FRAME = 0x13;
constexpr uint8_t STATUS2_CRYPTO1_ON = 0x08;

constexpr size_t FIFO_SIZE = 64;
// The timer runs at 13.56 MHz / (2 * 0xA9 + 1) = 40 kHz: 25 us a tick.
constexpr uint8_t TIMER_PRESCALER = 0xA9;
constexpr uint32_t TICKS_PER_MS = 40;
// Card answers to ISO 14443-3 and MIFARE commands come within a few ms.
constexpr uint32_t CARD_TIMEOUT_MS = 25;
constexpr uint32_t WAKE_TIMEOUT_MS = 3;

constexpr uint8_t WUPA = 0x52;
constexpr uint8_t SEL_CL1 = 0x93;
constexpr uint8_t CASCADE_TAG = 0x88;

// ISO 14443-3 CRC_A, low byte first.
void crcA(const uint8_t *data, size_t len, uint8_t *out) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i] ^ (uint8_t)crc;
        b ^= b << 4;
        crc = (crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4);
    }
    out[0] = crc & 0xFF;
    out[1] = crc >> 8;
}

}  // namespace

void Rc522::attach(SPIClass &spi, int sck, int miso, int mosi, int ss, int rst) {
    spi_ = &spi;
    ss_ = ss;
    rst_ = rst;
    pinMode(ss_, OUTPUT);
    digitalWrite(ss_, HIGH);
#if defined(ARDUINO_ARCH_SAMD)
    spi_->begin();                          // fixed pins: SCK, MISO, MOSI
#else
    spi_->begin(sck, miso, mosi, -1);
#endif
}

void Rc522::end() {
    if (ok_) clearBits(TxControlReg, 0x03);     // antenna off
    ok_ = false;
    isoDep_.reset();
    if (spi_) spi_->end();
}

uint8_t Rc522::readReg(uint8_t reg) {
    spi_->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(ss_, LOW);
    spi_->transfer(0x80 | (reg << 1));
    uint8_t value = spi_->transfer(0);
    digitalWrite(ss_, HIGH);
    spi_->endTransaction();
    return value;
}

// Each byte clocked out names the register again for the next read.
void Rc522::readFifo(uint8_t *out, size_t len) {
    if (!len) return;
    const uint8_t address = 0x80 | (FIFODataReg << 1);
    spi_->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(ss_, LOW);
    spi_->transfer(address);
    for (size_t i = 0; i < len; i++) out[i] = spi_->transfer(i + 1 < len ? address : 0);
    digitalWrite(ss_, HIGH);
    spi_->endTransaction();
}

void Rc522::writeReg(uint8_t reg, uint8_t value) { writeReg(reg, &value, 1); }

void Rc522::writeReg(uint8_t reg, const uint8_t *data, size_t len) {
    spi_->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(ss_, LOW);
    spi_->transfer(reg << 1);
    for (size_t i = 0; i < len; i++) spi_->transfer(data[i]);
    digitalWrite(ss_, HIGH);
    spi_->endTransaction();
}

void Rc522::setTimeout(uint32_t ms) {
    uint32_t ticks = min<uint32_t>(max<uint32_t>(ms, 1) * TICKS_PER_MS, 0xFFFF);
    writeReg(TReloadRegH, ticks >> 8);
    writeReg(TReloadRegL, ticks & 0xFF);
}

bool Rc522::init() {
    ok_ = false;
    isoDep_.reset();
    failures_ = 0;
    if (rst_ >= 0) {
        pinMode(rst_, OUTPUT);
        digitalWrite(rst_, LOW);
        delay(2);
        digitalWrite(rst_, HIGH);
        delay(50);
    }
    writeReg(CommandReg, CMD_SOFT_RESET);
    delay(50);
    for (int i = 0; i < 20 && (readReg(CommandReg) & 0x10); i++) delay(5);   // PowerDown
    uint8_t version = readReg(VersionReg);
    if (version == 0x00 || version == 0xFF) return false;
    switch (version) {
    case 0x91: firmware_ = "1.0"; break;
    case 0x92: firmware_ = "2.0"; break;
    case 0x88: firmware_ = "FM17522"; break;
    default: {
        char text[16];
        snprintf(text, sizeof(text), "0x%02X", version);
        firmware_ = text;
    }
    }
    version_ = version;
    writeReg(TxModeReg, 0x00);                  // 106 kbps, no hardware CRC
    writeReg(RxModeReg, 0x00);
    writeReg(ModWidthReg, 0x26);
    writeReg(TModeReg, 0x80);                   // timer starts when a frame is sent
    writeReg(TPrescalerReg, TIMER_PRESCALER);
    setTimeout(CARD_TIMEOUT_MS);
    writeReg(TxASKReg, 0x40);                   // 100 % ASK
    writeReg(ModeReg, 0x3D);                    // CRC preset 0x6363
    writeReg(RFCfgReg, 0x70);                   // 48 dB receiver gain: best range
    setBits(TxControlReg, 0x03);                // antenna on
    delay(5);
    ok_ = true;
    return true;
}

bool Rc522::chipAnswers() { return readReg(VersionReg) == version_; }

int Rc522::transceive(uint8_t command, const uint8_t *data, size_t len, uint8_t *out, size_t max,
                      uint8_t txBits, uint8_t *rxBits) {
    if (len > FIFO_SIZE) return -1;
    writeReg(CommandReg, CMD_IDLE);
    writeReg(ComIrqReg, 0x7F);                  // clear the interrupt bits
    writeReg(FIFOLevelReg, 0x80);               // flush the FIFO
    writeReg(FIFODataReg, data, len);
    writeReg(BitFramingReg, txBits & 0x07);
    writeReg(CommandReg, command);
    if (command == CMD_TRANSCEIVE) setBits(BitFramingReg, 0x80);   // StartSend
    // Transceive ends with a received frame; MFAuthent goes idle when done.
    // Either way the timer ends a wait for a card that does not answer.
    const uint8_t done = command == CMD_TRANSCEIVE ? IRQ_RX | IRQ_IDLE : IRQ_IDLE;
    const uint32_t start = millis();
    while (true) {
        uint8_t irq = readReg(ComIrqReg);
        if (irq & done) break;
        if (irq & IRQ_TIMER) {
            writeReg(CommandReg, CMD_IDLE);
            return -1;
        }
        // The timer always fires first; this only ends a wait on a chip
        // that stopped answering.
        if (millis() - start > 2000) {
            writeReg(CommandReg, CMD_IDLE);
            failures_++;
            return -1;
        }
    }
    uint8_t error = readReg(ErrorReg);
    if (error & ERR_FRAME) return -1;
    if (command != CMD_TRANSCEIVE) return 0;
    size_t n = readReg(FIFOLevelReg);
    if (n > max) return -1;
    readFifo(out, n);
    if (rxBits) *rxBits = readReg(ControlReg) & 0x07;
    if (error & ERR_COLLISION) return -1;
    return n;
}

int Rc522::transceiveCrc(const uint8_t *data, size_t len, uint8_t *out, size_t max) {
    uint8_t frame[FIFO_SIZE];
    if (len + 2 > sizeof(frame)) return -1;
    memcpy(frame, data, len);
    crcA(data, len, frame + len);
    uint8_t resp[FIFO_SIZE], rxBits = 0;
    int n = transceive(CMD_TRANSCEIVE, frame, len + 2, resp, sizeof(resp), 0, &rxBits);
    // A 4 bit answer is a MIFARE NAK.
    if (n < 3 || rxBits) return -1;
    uint8_t crc[2];
    crcA(resp, n - 2, crc);
    if (crc[0] != resp[n - 2] || crc[1] != resp[n - 1]) return -1;
    size_t count = min((size_t)(n - 2), max);
    memcpy(out, resp, count);
    return n - 2;
}

int Rc522::transceiveRaw(const uint8_t *data, size_t len, uint8_t *out, size_t max, bool crc,
                         uint8_t txBits, uint8_t *rxBits, uint32_t timeoutMs) {
    if (!ok_) return -1;
    uint8_t frame[FIFO_SIZE];
    if (len + 2 > sizeof(frame)) return -1;
    memcpy(frame, data, len);
    if (crc) crcA(data, len, frame + len);
    uint8_t resp[FIFO_SIZE], bits = 0;
    setTimeout(timeoutMs);
    int n = transceive(CMD_TRANSCEIVE, frame, len + (crc ? 2 : 0), resp, sizeof(resp), txBits, &bits);
    setTimeout(CARD_TIMEOUT_MS);
    if (n < 1) return -1;
    if (rxBits) *rxBits = bits;
    // A short frame (a 4 bit ACK or NAK) carries no CRC.
    if (crc && !bits) {
        if (n < 3) return -1;
        uint8_t check[2];
        crcA(resp, n - 2, check);
        if (check[0] != resp[n - 2] || check[1] != resp[n - 1]) return -1;
        n -= 2;
    }
    if ((size_t)n > max) return -1;
    memcpy(out, resp, n);
    return n;
}

bool Rc522::listTarget(NfcTarget &target) {
    if (!ok_) return false;
    if (!chipAnswers()) {
        failures_++;
        return false;
    }
    failures_ = 0;
    isoDep_.reset();
    clearBits(Status2Reg, STATUS2_CRYPTO1_ON);
    // WUPA, not REQA: it also wakes a card halted by a release or by a
    // refused MIFARE authentication, as the reader's reselect needs.
    setTimeout(WAKE_TIMEOUT_MS);
    uint8_t atqa[2], rxBits = 0;
    int n = transceive(CMD_TRANSCEIVE, &WUPA, 1, atqa, sizeof(atqa), 7, &rxBits);
    setTimeout(CARD_TIMEOUT_MS);
    if (n != 2 || rxBits) return false;
    target = NfcTarget();
    target.atqa[0] = atqa[1];               // as the PN532 reports it
    target.atqa[1] = atqa[0];
    if (!select(target)) return false;
    // ISO-DEP cards are activated, as the PN532 does by default.
    if (autoIsoDep_ && (target.sak & 0x20) && !(target.sak & 0x08) && isoDep_.activate()) {
        target.atsLen = min(isoDep_.atsLen(), sizeof(target.ats));
        memcpy(target.ats, isoDep_.ats(), target.atsLen);
    }
    return true;
}

// Anticollision and select through the cascade levels. One card at a time:
// a collision (two cards on the reader) fails the select.
bool Rc522::select(NfcTarget &target) {
    for (int level = 0; level < 3; level++) {
        uint8_t cmd[7] = {(uint8_t)(SEL_CL1 + 2 * level), 0x20};
        uint8_t uid[5];
        if (transceive(CMD_TRANSCEIVE, cmd, 2, uid, sizeof(uid)) != 5) return false;
        if ((uid[0] ^ uid[1] ^ uid[2] ^ uid[3]) != uid[4]) return false;
        cmd[1] = 0x70;
        memcpy(cmd + 2, uid, 5);
        uint8_t sak;
        if (transceiveCrc(cmd, sizeof(cmd), &sak, 1) != 1) return false;
        if (sak & 0x04) {                   // UID not complete
            if (uid[0] != CASCADE_TAG || target.uidLen + 3 > sizeof(target.uid)) return false;
            memcpy(target.uid + target.uidLen, uid + 1, 3);
            target.uidLen += 3;
            continue;
        }
        memcpy(target.uid + target.uidLen, uid, 4);
        target.uidLen += 4;
        target.sak = sak;
        return true;
    }
    return false;
}

int Rc522::transceiveFrame(const uint8_t *data, size_t len, uint8_t *out, size_t max, uint32_t timeoutMs) {
    setTimeout(timeoutMs);
    int n = transceiveCrc(data, len, out, max);
    setTimeout(CARD_TIMEOUT_MS);
    return n;
}

int Rc522::exchange(const uint8_t *data, size_t len, uint8_t *out, size_t expect, uint32_t timeoutMs) {
    (void)timeoutMs;    // the card's own timing decides
    if (!ok_) return -1;
    if (isoDep_.active()) return isoDep_.exchange(data, len, out, expect);
    if (len == 12 && (data[0] == 0x60 || data[0] == 0x61)) {
        // {auth, block, key[6], uid[4]} is exactly what MFAuthent takes.
        if (transceive(CMD_MF_AUTHENT, data, len, nullptr, 0) < 0) return -1;
        return (readReg(Status2Reg) & STATUS2_CRYPTO1_ON) ? 0 : -1;
    }
    return transceiveCrc(data, len, out, expect);
}

void Rc522::release() {
    if (!ok_) return;
    uint8_t resp[4];
    if (isoDep_.active()) {
        isoDep_.deselect();
    } else {
        // HLTA has no answer; under Crypto1 it goes out encrypted, as it must.
        const uint8_t halt[] = {0x50, 0x00};
        setTimeout(WAKE_TIMEOUT_MS);
        transceiveCrc(halt, sizeof(halt), resp, sizeof(resp));
        setTimeout(CARD_TIMEOUT_MS);
    }
    clearBits(Status2Reg, STATUS2_CRYPTO1_ON);
}
