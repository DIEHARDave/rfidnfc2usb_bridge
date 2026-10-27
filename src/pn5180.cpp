// Host interface per the PN5180 datasheet (rev. 3.6), section 11: every
// SPI frame is sent with NSS low, then BUSY goes high; NSS goes high and
// the next frame waits until BUSY is low again.
#include "pn5180.h"

namespace {

constexpr uint32_t SPI_CLOCK = 4000000;

// Host interface commands
constexpr uint8_t CMD_WRITE_REGISTER = 0x00;
constexpr uint8_t CMD_WRITE_REGISTER_OR_MASK = 0x01;
constexpr uint8_t CMD_WRITE_REGISTER_AND_MASK = 0x02;
constexpr uint8_t CMD_READ_REGISTER = 0x04;
constexpr uint8_t CMD_READ_EEPROM = 0x07;
constexpr uint8_t CMD_SEND_DATA = 0x09;
constexpr uint8_t CMD_READ_DATA = 0x0A;
constexpr uint8_t CMD_MIFARE_AUTHENTICATE = 0x0C;
constexpr uint8_t CMD_LOAD_RF_CONFIG = 0x11;
constexpr uint8_t CMD_RF_ON = 0x16;
constexpr uint8_t CMD_RF_OFF = 0x17;

// Registers
constexpr uint8_t SYSTEM_CONFIG = 0x00;
constexpr uint8_t IRQ_STATUS = 0x02;
constexpr uint8_t CRC_RX_CONFIG = 0x12;
constexpr uint8_t RX_STATUS = 0x13;
constexpr uint8_t CRC_TX_CONFIG = 0x19;
constexpr uint8_t RF_STATUS = 0x1D;

constexpr uint32_t IRQ_RX = 1UL << 0;
constexpr uint32_t IRQ_IDLE = 1UL << 2;
constexpr uint32_t IRQ_TX_RFON = 1UL << 9;
constexpr uint32_t IRQ_GENERAL_ERROR = 1UL << 17;
// RX_STATUS: integrity error, protocol error, collision.
constexpr uint32_t RX_ERRORS = 0x7UL << 16;
constexpr uint32_t SYSTEM_CONFIG_MFC_CRYPTO_ON = 1UL << 6;
constexpr uint8_t RF_STATE_WAIT_TRANSMIT = 1;

constexpr uint8_t EEPROM_FIRMWARE_VERSION = 0x12;
const uint8_t RF_CONFIG_14443A_106[] = {0x00, 0x80};
const uint8_t RF_CONFIG_15693_26[] = {0x0D, 0x8D};

constexpr uint32_t BUSY_TIMEOUT_MS = 100;
constexpr uint32_t CARD_TIMEOUT_MS = 25;
constexpr uint32_t WAKE_TIMEOUT_MS = 5;
// ISO 15693 at 26 kbps: a multiple block read takes over 10 ms.
constexpr uint32_t VICINITY_TIMEOUT_MS = 40;
constexpr size_t RX_MAX = 260;

constexpr uint8_t WUPA = 0x52;
constexpr uint8_t SEL_CL1 = 0x93;
constexpr uint8_t CASCADE_TAG = 0x88;

}  // namespace

void Pn5180::attach(SPIClass &spi, int sck, int miso, int mosi, int nss, int busy, int rst) {
    spi_ = &spi;
    nss_ = nss;
    busy_ = busy;
    rst_ = rst;
    pinMode(nss_, OUTPUT);
    digitalWrite(nss_, HIGH);
    pinMode(busy_, INPUT);
#if defined(ARDUINO_ARCH_SAMD)
    spi_->begin();                          // fixed pins: SCK, MISO, MOSI
#else
    spi_->begin(sck, miso, mosi, -1);
#endif
}

void Pn5180::end() {
    if (ok_) rfOff();
    ok_ = false;
    isoDep_.reset();
    if (spi_) spi_->end();
}

bool Pn5180::waitBusy(int level, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (digitalRead(busy_) != level)
        if (millis() - start > timeoutMs) return false;
    return true;
}

bool Pn5180::command(const uint8_t *send, size_t len, uint8_t *recv, size_t recvLen) {
    if (!waitBusy(LOW, BUSY_TIMEOUT_MS)) {
        failures_++;
        return false;
    }
    spi_->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(nss_, LOW);
    for (size_t i = 0; i < len; i++) spi_->transfer(send[i]);
    bool ok = waitBusy(HIGH, 5);
    digitalWrite(nss_, HIGH);
    ok = waitBusy(LOW, BUSY_TIMEOUT_MS) && ok;
    if (ok && recvLen) {
        digitalWrite(nss_, LOW);
        for (size_t i = 0; i < recvLen; i++) recv[i] = spi_->transfer(0xFF);
        ok = waitBusy(HIGH, 5);
        digitalWrite(nss_, HIGH);
        ok = waitBusy(LOW, BUSY_TIMEOUT_MS) && ok;
    }
    spi_->endTransaction();
    if (!ok) {
        failures_++;
        return false;
    }
    failures_ = 0;
    return true;
}

bool Pn5180::writeRegister(uint8_t reg, uint32_t value) {
    const uint8_t cmd[] = {CMD_WRITE_REGISTER, reg, (uint8_t)value, (uint8_t)(value >> 8),
                           (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return command(cmd, sizeof(cmd));
}

bool Pn5180::orRegister(uint8_t reg, uint32_t mask) {
    const uint8_t cmd[] = {CMD_WRITE_REGISTER_OR_MASK, reg, (uint8_t)mask, (uint8_t)(mask >> 8),
                           (uint8_t)(mask >> 16), (uint8_t)(mask >> 24)};
    return command(cmd, sizeof(cmd));
}

bool Pn5180::andRegister(uint8_t reg, uint32_t mask) {
    const uint8_t cmd[] = {CMD_WRITE_REGISTER_AND_MASK, reg, (uint8_t)mask, (uint8_t)(mask >> 8),
                           (uint8_t)(mask >> 16), (uint8_t)(mask >> 24)};
    return command(cmd, sizeof(cmd));
}

bool Pn5180::readRegister(uint8_t reg, uint32_t &value) {
    const uint8_t cmd[] = {CMD_READ_REGISTER, reg};
    uint8_t buf[4];
    if (!command(cmd, sizeof(cmd), buf, sizeof(buf))) return false;
    value = buf[0] | (buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return true;
}

bool Pn5180::init() {
    ok_ = false;
    failures_ = 0;
    rf_ = Rf::Off;
    crc_ = -1;
    isoDep_.reset();
    if (rst_ >= 0) {
        pinMode(rst_, OUTPUT);
        digitalWrite(rst_, LOW);
        delay(10);
        digitalWrite(rst_, HIGH);
        delay(10);
        uint32_t start = millis(), irq = 0;
        while (millis() - start < 100 && !(readRegister(IRQ_STATUS, irq) && (irq & IRQ_IDLE))) delay(1);
    }
    const uint8_t cmd[] = {CMD_READ_EEPROM, EEPROM_FIRMWARE_VERSION, 2};
    uint8_t version[2];
    if (!command(cmd, sizeof(cmd), version, sizeof(version))) return false;
    if ((version[0] == 0xFF && version[1] == 0xFF) || (version[0] == 0 && version[1] == 0)) return false;
    char text[16];
    snprintf(text, sizeof(text), "%d.%d", version[1], version[0]);
    firmware_ = text;
    if (!clearIrq()) return false;
    ok_ = true;
    return true;
}

bool Pn5180::rfMode(Rf mode) {
    if (rf_ == mode) return true;
    if (rf_ != Rf::Off) {
        rfOff();
        delay(2);
    }
    const uint8_t *config = mode == Rf::Iso15693 ? RF_CONFIG_15693_26 : RF_CONFIG_14443A_106;
    const uint8_t load[] = {CMD_LOAD_RF_CONFIG, config[0], config[1]};
    const uint8_t on[] = {CMD_RF_ON, 0x00};
    if (!command(load, sizeof(load)) || !command(on, sizeof(on))) return false;
    uint32_t start = millis(), irq = 0;
    while (millis() - start < 50 && !(readRegister(IRQ_STATUS, irq) && (irq & IRQ_TX_RFON))) delay(1);
    clearIrq();
    rf_ = mode;
    crc_ = -1;
    // Cards need a few ms of field before their first command.
    delay(5);
    return true;
}

void Pn5180::rfOff() {
    const uint8_t off[] = {CMD_RF_OFF, 0x00};
    command(off, sizeof(off));
    clearIrq();
    rf_ = Rf::Off;
    isoDep_.reset();
}

void Pn5180::setCrc(bool on) {
    if (crc_ == (int)on) return;
    if (on) {
        orRegister(CRC_RX_CONFIG, 0x01);
        orRegister(CRC_TX_CONFIG, 0x01);
    } else {
        andRegister(CRC_RX_CONFIG, 0xFFFFFFFE);
        andRegister(CRC_TX_CONFIG, 0xFFFFFFFE);
    }
    crc_ = on;
}

int Pn5180::transceive(const uint8_t *data, size_t len, uint8_t *out, size_t max, uint32_t timeoutMs,
                       uint8_t txBits, uint8_t *rxBits) {
    uint8_t send[2 + 64];
    if (len > sizeof(send) - 2) return -1;
    // Idle, then the transceive state machine, which waits to transmit.
    if (!clearIrq() || !andRegister(SYSTEM_CONFIG, 0xFFFFFFF8) || !orRegister(SYSTEM_CONFIG, 0x00000003))
        return -1;
    uint32_t start = millis(), value = 0;
    while (true) {
        if (!readRegister(RF_STATUS, value)) return -1;
        if (((value >> 24) & 0x07) == RF_STATE_WAIT_TRANSMIT) break;
        if (millis() - start > 5) return -1;
    }
    send[0] = CMD_SEND_DATA;
    send[1] = txBits & 0x07;
    memcpy(send + 2, data, len);
    if (!command(send, len + 2)) return -1;
    start = millis();
    while (true) {
        if (!readRegister(IRQ_STATUS, value)) return -1;
        if (value & IRQ_RX) break;
        if ((value & IRQ_GENERAL_ERROR) || millis() - start > timeoutMs) return -1;
    }
    uint32_t status;
    if (!readRegister(RX_STATUS, status) || (status & RX_ERRORS)) return -1;
    size_t n = status & 0x1FF;
    if (!n || n > RX_MAX) return -1;
    uint8_t buf[RX_MAX];
    const uint8_t read[] = {CMD_READ_DATA, 0x00};
    if (!command(read, sizeof(read), buf, n)) return -1;
    clearIrq();
    if (rxBits) *rxBits = (status >> 12) & 0x07;
    if (n > max) return -1;
    memcpy(out, buf, n);
    return n;
}

int Pn5180::transceiveFrame(const uint8_t *data, size_t len, uint8_t *out, size_t max, uint32_t timeoutMs) {
    setCrc(true);
    uint8_t rxBits = 0;
    int n = transceive(data, len, out, max, timeoutMs, 0, &rxBits);
    return n < 1 || rxBits ? -1 : n;
}

int Pn5180::transceiveRaw(const uint8_t *data, size_t len, uint8_t *out, size_t max, bool crc,
                          uint8_t txBits, uint8_t *rxBits, uint32_t timeoutMs) {
    if (!ok_) return -1;
    if (rf_ == Rf::Off && !rfMode(Rf::Iso14443A)) return -1;
    setCrc(crc);
    uint8_t bits = 0;
    int n = transceive(data, len, out, max, timeoutMs, txBits, &bits);
    if (n < 1) return -1;
    if (rxBits) *rxBits = bits;
    return n;
}

bool Pn5180::listTarget(NfcTarget &target) {
    if (!ok_) return false;
    isoDep_.reset();
    if (!rfMode(Rf::Iso14443A)) return false;
    andRegister(SYSTEM_CONFIG, ~SYSTEM_CONFIG_MFC_CRYPTO_ON);
    setCrc(false);
    // WUPA, not REQA: it also wakes a card halted by a release or by a
    // refused MIFARE authentication, as the reader's reselect needs.
    uint8_t atqa[2], rxBits = 0;
    if (transceive(&WUPA, 1, atqa, sizeof(atqa), WAKE_TIMEOUT_MS, 7, &rxBits) != 2 || rxBits) return false;
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
bool Pn5180::select(NfcTarget &target) {
    for (int level = 0; level < 3; level++) {
        uint8_t cmd[7] = {(uint8_t)(SEL_CL1 + 2 * level), 0x20};
        uint8_t uid[5];
        setCrc(false);
        if (transceive(cmd, 2, uid, sizeof(uid), CARD_TIMEOUT_MS) != 5) return false;
        if ((uid[0] ^ uid[1] ^ uid[2] ^ uid[3]) != uid[4]) return false;
        cmd[1] = 0x70;
        memcpy(cmd + 2, uid, 5);
        uint8_t sak;
        setCrc(true);
        if (transceive(cmd, sizeof(cmd), &sak, 1, CARD_TIMEOUT_MS) != 1) return false;
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

int Pn5180::exchange(const uint8_t *data, size_t len, uint8_t *out, size_t expect, uint32_t timeoutMs) {
    (void)timeoutMs;    // the card's own timing decides
    if (!ok_ || rf_ != Rf::Iso14443A) return -1;
    if (isoDep_.active()) return isoDep_.exchange(data, len, out, expect);
    if (len == 12 && (data[0] == 0x60 || data[0] == 0x61)) {
        // {auth, block, key[6], uid[4]} -> key, key type, block, uid.
        uint8_t cmd[13] = {CMD_MIFARE_AUTHENTICATE};
        memcpy(cmd + 1, data + 2, 6);
        cmd[7] = data[0];
        cmd[8] = data[1];
        memcpy(cmd + 9, data + 8, 4);
        uint8_t status;
        if (!command(cmd, sizeof(cmd), &status, 1)) return -1;
        return status == 0 ? 0 : -1;
    }
    setCrc(true);
    uint8_t buf[64], rxBits = 0;
    int n = transceive(data, len, buf, sizeof(buf), CARD_TIMEOUT_MS, 0, &rxBits);
    // A 4 bit answer is a MIFARE NAK.
    if (n < 1 || rxBits) return -1;
    size_t count = min((size_t)n, expect);
    memcpy(out, buf, count);
    return count;
}

void Pn5180::release() {
    if (!ok_ || rf_ != Rf::Iso14443A) return;
    if (isoDep_.active()) {
        isoDep_.deselect();
    } else {
        // HLTA has no answer; under Crypto1 it goes out encrypted, as it must.
        const uint8_t halt[] = {0x50, 0x00};
        uint8_t resp[4];
        setCrc(true);
        transceive(halt, sizeof(halt), resp, sizeof(resp), 2);
    }
    andRegister(SYSTEM_CONFIG, ~SYSTEM_CONFIG_MFC_CRYPTO_ON);
}

bool Pn5180::inventory15693(NfcTarget &target) {
    if (!ok_ || !rfMode(Rf::Iso15693)) return false;
    setCrc(true);
    // High data rate, inventory, one slot, no mask.
    const uint8_t inventory[] = {0x26, 0x01, 0x00};
    uint8_t resp[16];
    int n = transceive(inventory, sizeof(inventory), resp, sizeof(resp), VICINITY_TIMEOUT_MS);
    if (n < 10 || (resp[0] & 0x01)) return false;
    target = NfcTarget();
    target.iso15693 = true;
    target.uidLen = 8;
    for (int i = 0; i < 8; i++) target.uid[i] = resp[9 - i];    // sent LSB first
    return true;
}

int Pn5180::exchange15693(const uint8_t *request, size_t len, uint8_t *out, size_t max) {
    if (!ok_ || rf_ != Rf::Iso15693) return -1;
    setCrc(true);
    uint8_t resp[RX_MAX];
    int n = transceive(request, len, resp, sizeof(resp), VICINITY_TIMEOUT_MS);
    if (n < 1 || (resp[0] & 0x01)) return -1;
    size_t count = min((size_t)(n - 1), max);
    memcpy(out, resp + 1, count);
    return count;
}
