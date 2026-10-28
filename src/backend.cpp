#include "backend.h"

namespace {

// How often a missing chip is looked for again. A probe stops the host link
// for up to about half a second, so not too often.
constexpr uint32_t RETRY_MS = 5000;
constexpr int MAX_FAILURES = 5;
// Per command while probing for a PN532 on UART that may not be there.
constexpr uint32_t PROBE_TIMEOUT_MS = 100;

// The bus objects behind the configured pins. On the RP2040 / RP2350 the
// pins choose the hardware block; ESP32 and SAMD use the first one.
#if defined(ARDUINO_ARCH_RP2040)
TwoWire &wireFor(const Config &c) { return rpI2cBlock(c.sda) ? Wire1 : Wire; }
SerialUART &uartFor(const Config &c) { return rpUartBlock(c.tx) ? Serial2 : Serial1; }
SPIClass &spiFor(const Config &c) { return rpSpiBlock(c.sck) ? SPI1 : SPI; }
#else
TwoWire &wireFor(const Config &) { return Wire; }
HardwareSerial &uartFor(const Config &) { return Serial1; }
SPIClass &spiFor(const Config &) { return SPI; }
#endif

}  // namespace

void Backend::begin(const Config &config) {
    config_ = config;
    detect();
}

bool Backend::tryChip(ChipSetting chip, bool probing) {
    chip_ = nullptr;
    switch (chip) {
    case ChipSetting::Pn532I2C:
        if (config_.sda < 0 || config_.scl < 0) return false;
        pn532_.attachI2C(wireFor(config_), config_.sda, config_.scl);
        // With a line held low every transfer waits for its timeout (about
        // a second each on the classic ESP32), stalling the host link.
        if (!pn532_.pulledUp()) {
            stuck_ = nullptr;
            pn532_.end();
            break;
        }
        stuck_ = pn532_.stuckLine();
        if (!stuck_ && (!probing || pn532_.present()) && pn532_.init()) chip_ = &pn532_;
        else pn532_.end();
        break;
    case ChipSetting::Pn532Uart:
        if (config_.rx < 0 || config_.tx < 0) return false;
        pn532_.attachUart(uartFor(config_), config_.rx, config_.tx);
        if (pn532_.init(probing ? PROBE_TIMEOUT_MS : 500)) chip_ = &pn532_;
        else pn532_.end();
        break;
    case ChipSetting::Rc522:
        if (config_.sck < 0 || config_.miso < 0 || config_.mosi < 0 || config_.ss < 0) return false;
        rc522_.attach(spiFor(config_), config_.sck, config_.miso, config_.mosi, config_.ss, config_.rst);
        if (rc522_.init()) chip_ = &rc522_;
        else rc522_.end();
        break;
    case ChipSetting::Pn5180:
        if (config_.sck < 0 || config_.miso < 0 || config_.mosi < 0 || config_.ss < 0 || config_.busy < 0)
            return false;
        pn5180_.attach(spiFor(config_), config_.sck, config_.miso, config_.mosi, config_.ss, config_.busy, config_.rst);
        if (pn5180_.init()) chip_ = &pn5180_;
        else pn5180_.end();
        break;
    case ChipSetting::Auto:
        break;
    }
    if (chip_) found_ = chip;
    return chip_ != nullptr;
}

bool Backend::detect() {
    nextTry_ = millis() + RETRY_MS;
    if (config_.chip != ChipSetting::Auto) return tryChip(config_.chip, false);
    // The chip found last first, then the rest. The PN5180 goes before the
    // RC522: the RC522 probe's register writes mean nothing to a PN5180,
    // while the PN5180 probe fails fast on an RC522 (no BUSY).
    const ChipSetting order[] = {ChipSetting::Pn532I2C, ChipSetting::Pn532Uart, ChipSetting::Pn5180,
                                 ChipSetting::Rc522};
    if (found_ != ChipSetting::Auto && tryChip(found_, true)) return true;
    for (ChipSetting chip : order)
        if (chip != found_ && tryChip(chip, true)) return true;
    return false;
}

void Backend::service() {
    if (chip_ && chip_->ready() && chip_->failures() < MAX_FAILURES) return;
    if (chip_ && chip_->ready()) {
        // Stopped answering: start it again at once.
        chip_->end();
        detect();
        return;
    }
    if ((int32_t)(millis() - nextTry_) >= 0) detect();
}

String Backend::describe() const {
    if (!chip_ || !chip_->ready()) {
        String text = config_.chip == ChipSetting::Auto
                          ? String("no reader found")
                          : String("no ") + chipSettingName(config_.chip) + " answering";
        if (stuck_) {
            int pin = strcmp(stuck_, "SCL") == 0 ? config_.scl : config_.sda;
            text += String(" (I2C ") + stuck_ + ", GPIO " + String(pin) +
                    ", is held low: check the wiring, or use the PN532 over UART)";
        }
        return text;
    }
    return String(chip_->chipName()) + " v" + chip_->firmware() + " (" + chip_->busName() + ")";
}
