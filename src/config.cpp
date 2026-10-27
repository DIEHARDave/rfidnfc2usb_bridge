#include "config.h"

#if defined(ARDUINO_ARCH_SAMD)
#include <FlashStorage.h>
#else
#include <Preferences.h>
#endif

namespace {

#if defined(ARDUINO_ARCH_SAMD)
// SAMD has no NVS: the JSON goes in a flash row of the program image, which
// a firmware upload erases. magic tells a saved row from an erased one.
struct StoredConfig {
    uint32_t magic;
    char text[384];
};
constexpr uint32_t STORED_MAGIC = 0x6E663275;   // "nf2u"
FlashStorage(configStore, StoredConfig);
#else
const char *const NVS_NAMESPACE = "nfc2usb";
const char *const NVS_KEY = "config";
#endif

// Default pins per board. The I2C, UART and SPI pins do not overlap, so
// "auto" can probe every chip without one probe disturbing another reader.
#if defined(ARDUINO_ARCH_SAMD)
// Seeed XIAO SAMD21: I2C on D4/D5, UART on D6 (TX) / D7 (RX) and SPI on
// D8-D10 are fixed; the yellow LED (pin 13) is lit when low. SS, RST and
// BUSY take D3, D2 and D1.
constexpr int SDA = 4, SCL = 5, RX = 7, TX = 6;
constexpr int SCK = 8, MISO = 9, MOSI = 10, SS = 3, RST = 2, BUSY = 1;
constexpr int LED = 13;
constexpr bool LED_LOW = true;
#elif CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3 DevKitC (its RGB LED is addressable, so no status LED).
constexpr int SDA = 8, SCL = 9, RX = 18, TX = 17;
constexpr int SCK = 12, MISO = 13, MOSI = 11, SS = 10, RST = 14, BUSY = 15;
constexpr int LED = -1;
constexpr bool LED_LOW = false;
#elif CONFIG_IDF_TARGET_ESP32S2
// Wemos/Lolin S2 Mini: the blue LED is on GPIO 15.
constexpr int SDA = 8, SCL = 9, RX = 18, TX = 17;
constexpr int SCK = 12, MISO = 13, MOSI = 11, SS = 10, RST = 14, BUSY = 16;
constexpr int LED = 15;
constexpr bool LED_LOW = false;
#elif CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 SuperMini: the blue LED is on GPIO 8 (lit when low). I2C on 6/7
// as in LaneTag; GPIO 2, 8 and 9 are strapping pins and stay free.
constexpr int SDA = 6, SCL = 7, RX = 20, TX = 21;
constexpr int SCK = 4, MISO = 5, MOSI = 3, SS = 1, RST = 0, BUSY = 10;
constexpr int LED = 8;
constexpr bool LED_LOW = true;
#elif CONFIG_IDF_TARGET_ESP32C6
// ESP32-C6 DevKitC (its RGB LED is addressable, so no status LED).
constexpr int SDA = 6, SCL = 7, RX = 4, TX = 5;
constexpr int SCK = 19, MISO = 20, MOSI = 18, SS = 21, RST = 22, BUSY = 23;
constexpr int LED = -1;
constexpr bool LED_LOW = false;
#else
// Classic ESP32 dev boards (DevKitC, DOIT, NodeMCU-32S): LED on GPIO 2.
constexpr int SDA = 21, SCL = 22, RX = 16, TX = 17;
constexpr int SCK = 18, MISO = 19, MOSI = 23, SS = 5, RST = 4, BUSY = 27;
constexpr int LED = 2;
constexpr bool LED_LOW = false;
#endif

const char *const CHIP_NAMES[] = {"auto", "pn532_i2c", "pn532_uart", "rc522", "pn5180"};

void setDefaults(Config &config) {
    config = Config();
    config.sda = SDA;
    config.scl = SCL;
    config.rx = RX;
    config.tx = TX;
    config.sck = SCK;
    config.miso = MISO;
    config.mosi = MOSI;
    config.ss = SS;
    config.rst = RST;
    config.busy = BUSY;
    config.led = LED;
    config.ledActiveLow = LED_LOW;
}

// A pin number, -1 for "not wired", or the old value when value is absent.
bool pin(JsonVariantConst value, int &out) {
    if (value.isNull()) return true;
    if (!value.is<int>()) return false;
    int n = value.as<int>();
    if (n < -1 || n > 48) return false;
    out = n;
    return true;
}

}  // namespace

const char *chipSettingName(ChipSetting chip) {
    size_t i = (size_t)chip;
    return i < sizeof(CHIP_NAMES) / sizeof(CHIP_NAMES[0]) ? CHIP_NAMES[i] : "?";
}

#if defined(ARDUINO_ARCH_SAMD)
namespace {

String readStored() {
    StoredConfig stored = configStore.read();
    if (stored.magic != STORED_MAGIC) return "";
    stored.text[sizeof(stored.text) - 1] = 0;
    return stored.text;
}

void writeStored(const String &text) {
    StoredConfig stored = {};
    if (text.length() >= sizeof(stored.text)) return;
    stored.magic = text.length() ? STORED_MAGIC : 0;
    memcpy(stored.text, text.c_str(), text.length());
    configStore.write(stored);
}

}  // namespace

void loadConfig(Config &config) {
    setDefaults(config);
    String text = readStored();
    if (!text.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, text)) return;
    Config loaded = config;
    if (configFromJson(loaded, doc.as<JsonObjectConst>()).length() == 0) config = loaded;
}

void saveConfig(const Config &config) {
    JsonDocument doc;
    configToJson(config, doc.to<JsonObject>());
    String text;
    serializeJson(doc, text);
    writeStored(text);
}

void resetConfig() { writeStored(""); }
#else
void loadConfig(Config &config) {
    setDefaults(config);
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) return;
    String text = prefs.getString(NVS_KEY, "");
    prefs.end();
    if (!text.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, text)) return;
    Config loaded = config;
    if (configFromJson(loaded, doc.as<JsonObjectConst>()).length() == 0) config = loaded;
}

void saveConfig(const Config &config) {
    JsonDocument doc;
    configToJson(config, doc.to<JsonObject>());
    String text;
    serializeJson(doc, text);
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return;
    prefs.putString(NVS_KEY, text);
    prefs.end();
}

void resetConfig() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return;
    prefs.clear();
    prefs.end();
}
#endif

void configToJson(const Config &config, JsonObject out) {
    out["chip"] = chipSettingName(config.chip);
    out["sda"] = config.sda;
    out["scl"] = config.scl;
    out["rx"] = config.rx;
    out["tx"] = config.tx;
    out["sck"] = config.sck;
    out["miso"] = config.miso;
    out["mosi"] = config.mosi;
    out["ss"] = config.ss;
    out["rst"] = config.rst;
    out["busy"] = config.busy;
    out["led"] = config.led;
    out["led_active_low"] = config.ledActiveLow;
}

String configFromJson(Config &config, JsonObjectConst in) {
    Config next = config;
    JsonVariantConst chip = in["chip"];
    if (!chip.isNull()) {
        const char *name = chip.as<const char *>();
        bool found = false;
        for (size_t i = 0; name && i < sizeof(CHIP_NAMES) / sizeof(CHIP_NAMES[0]); i++) {
            if (strcmp(name, CHIP_NAMES[i]) == 0) {
                next.chip = (ChipSetting)i;
                found = true;
            }
        }
        if (!found) return "chip must be auto, pn532_i2c, pn532_uart, rc522 or pn5180";
    }
    const char *keys[] = {"sda", "scl", "rx", "tx", "sck", "miso", "mosi", "ss", "rst", "busy", "led"};
    int *pins[] = {&next.sda, &next.scl, &next.rx, &next.tx, &next.sck, &next.miso,
                   &next.mosi, &next.ss, &next.rst, &next.busy, &next.led};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!pin(in[keys[i]], *pins[i])) return String(keys[i]) + " must be a GPIO number or -1";
    JsonVariantConst low = in["led_active_low"];
    if (!low.isNull()) {
        if (!low.is<bool>()) return "led_active_low must be true or false";
        next.ledActiveLow = low.as<bool>();
    }

    bool i2c = next.chip == ChipSetting::Auto || next.chip == ChipSetting::Pn532I2C;
    bool uart = next.chip == ChipSetting::Pn532Uart;
    bool spi = next.chip == ChipSetting::Rc522 || next.chip == ChipSetting::Pn5180;
    if (i2c && next.chip != ChipSetting::Auto && (next.sda < 0 || next.scl < 0))
        return "the PN532 on I2C needs sda and scl";
    if (uart && (next.rx < 0 || next.tx < 0)) return "the PN532 on UART needs rx and tx";
    if (spi && (next.sck < 0 || next.miso < 0 || next.mosi < 0 || next.ss < 0))
        return "the RC522 and PN5180 need sck, miso, mosi and ss";
    if (next.chip == ChipSetting::Pn5180 && next.busy < 0) return "the PN5180 needs busy";
    config = next;
    return "";
}
