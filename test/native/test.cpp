// Native test of the PN532 emulator against a mock card (see Makefile).
#include <deque>
#include <map>
#include <vector>

#include "backend.h"
#include "emulator.h"

uint32_t fakeMillis = 0;
EspClass ESP;
TwoWire Wire;
HardwareSerial Serial1;
SPIClass SPI;
std::map<std::string, std::string> fakeNvs;

// Bytes the host sends at a given fake time (for aborts).
struct Timed {
    uint32_t at;
    std::vector<uint8_t> bytes;
};

void (*pumpFn)() = nullptr;    // the simulator moves bytes to and from its pty

class FakeHost : public Stream {
public:
    std::deque<uint8_t> in;
    std::vector<uint8_t> out;
    std::vector<Timed> later;
    void release() {
        for (auto it = later.begin(); it != later.end();) {
            if (fakeMillis >= it->at) {
                in.insert(in.end(), it->bytes.begin(), it->bytes.end());
                it = later.erase(it);
            } else {
                ++it;
            }
        }
    }
    int available() override {
        if (pumpFn) pumpFn();
        release();
        return in.size();
    }
    size_t readBytes(uint8_t *buf, size_t len) override {
        size_t n = 0;
        while (n < len && !in.empty()) {
            buf[n++] = in.front();
            in.pop_front();
        }
        return n;
    }
    size_t write(const uint8_t *buf, size_t len) override {
        out.insert(out.end(), buf, buf + len);
        if (pumpFn) pumpFn();
        return len;
    }
};
#ifndef SIM
void delay(uint32_t ms) { fakeMillis += ms; }
#endif

enum class Card { None, Ntag, Classic, IsoDep };

class MockChip : public NfcChip {
public:
    Card card = Card::None;
    uint8_t mem[64][16] = {};
    bool authed = false;
    int writeStage = -1;
    bool lastCrc = true;
    uint8_t lastTxBits = 0;
    bool autoIsoDep = true;
    bool iso15693 = false;
    bool init() override { return true; }
    void end() override {}
    bool ready() const override { return true; }
    const char *chipName() const override { return "RC522"; }
    const char *busName() const override { return "SPI"; }
    String firmware() const override { return "2.0"; }
    void setAutoIsoDep(bool on) override { autoIsoDep = on; }
    bool listTarget(NfcTarget &t) override {
        t = NfcTarget();
        authed = false;
        if (card == Card::Ntag) {
            const uint8_t uid[] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
            memcpy(t.uid, uid, 7);
            t.uidLen = 7;
            t.atqa[1] = 0x44;
            return true;
        }
        if (card == Card::Classic) {
            const uint8_t uid[] = {0xDE, 0xAD, 0xBE, 0xEF};
            memcpy(t.uid, uid, 4);
            t.uidLen = 4;
            t.atqa[1] = 0x04;
            t.sak = 0x08;
            return true;
        }
        if (card == Card::IsoDep) {
            const uint8_t uid[] = {0x01, 0x02, 0x03, 0x04};
            memcpy(t.uid, uid, 4);
            t.uidLen = 4;
            t.atqa[1] = 0x04;
            t.sak = 0x20;
            if (autoIsoDep) {
                const uint8_t ats[] = {0x05, 0x78, 0x80, 0x70, 0x02};
                memcpy(t.ats, ats, 5);
                t.atsLen = 5;
            }
            return true;
        }
        return false;
    }
    int exchange(const uint8_t *d, size_t len, uint8_t *out, size_t expect, uint32_t) override {
        if (card == Card::IsoDep) {
            if (len >= 2 && d[0] == 0x00 && d[1] == 0xB0) {     // 256 bytes + 90 00
                for (int i = 0; i < 256; i++) out[i] = 0xAB;
                out[256] = 0x90;
                out[257] = 0x00;
                return 258;
            }
            memcpy(out, d, len);                               // echo + 90 00
            out[len] = 0x90;
            out[len + 1] = 0;
            return len + 2;
        }
        if ((d[0] == 0x60 || d[0] == 0x61) && len == 12) {
            static const uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            authed = memcmp(d + 2, key, 6) == 0;
            return authed ? 0 : -1;
        }
        if (d[0] == 0x30 && len == 2) {
            if (card == Card::Classic && !authed) return -1;
            for (int i = 0; i < 16; i++)
                out[i] = card == Card::Ntag ? mem[(d[1] + i / 4) % 64][i % 4] : mem[d[1]][i];
            return std::min<size_t>(16, expect);
        }
        return -1;
    }
    int transceiveRaw(const uint8_t *d, size_t len, uint8_t *out, size_t, bool crc, uint8_t txBits,
                      uint8_t *rxBits, uint32_t) override {
        lastCrc = crc;
        lastTxBits = txBits;
        auto ack = [&]() {
            out[0] = 0x0A;
            if (rxBits) *rxBits = 4;
            return 1;
        };
        if (writeStage >= 0) {                      // second step of A0
            if (len != 16) return -1;
            memcpy(mem[writeStage], d, 16);
            writeStage = -1;
            return ack();
        }
        if (d[0] == 0xA2 && len == 6 && card == Card::Ntag) {
            memcpy(mem[d[1]], d + 2, 4);
            return ack();
        }
        if (d[0] == 0xA0 && len == 2 && card == Card::Classic && authed) {
            writeStage = d[1];
            return ack();
        }
        if (d[0] == 0x26 && card == Card::Ntag) {
            out[0] = 0x44;
            out[1] = 0x00;
            if (rxBits) *rxBits = 0;
            return 2;
        }
        if (iso15693 && d[0] == 0x22) {
            out[0] = 0x00;
            out[1] = 0x11;
            out[2] = 0x22;
            if (rxBits) *rxBits = 0;
            return 3;
        }
        return -1;
    }
    void release() override {}
    int failures() const override { return 0; }
    bool hasIso15693() const override { return iso15693; }
    bool inventory15693(NfcTarget &t) override {
        if (!iso15693) return false;
        t = NfcTarget();
        t.iso15693 = true;
        t.uidLen = 8;
        const uint8_t uid[] = {0xE0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
        memcpy(t.uid, uid, 8);
        return true;
    }
};

MockChip mock;
// Backend for the test: the mock chip.
void Backend::begin(const Config &c) {
    config_ = c;
    chip_ = &mock;
}
void Backend::service() {}
String Backend::describe() const { return "RC522 v2.0 (SPI)"; }
// Never reached: the mock is not a PN532.
int Pn532::command(uint8_t, const uint8_t *, size_t, uint8_t *, size_t, uint32_t, bool) { return -1; }
bool Pn532::listTarget(NfcTarget &) { return false; }
int Pn532::exchange(const uint8_t *, size_t, uint8_t *, size_t, uint32_t) { return -1; }
void Pn532::release() {}
void Pn532::end() {}
bool Pn532::init(uint32_t) { return false; }
bool Rc522::init() { return false; }
void Rc522::end() {}
bool Rc522::listTarget(NfcTarget &) { return false; }
int Rc522::exchange(const uint8_t *, size_t, uint8_t *, size_t, uint32_t) { return -1; }
void Rc522::release() {}
int Rc522::transceiveRaw(const uint8_t *, size_t, uint8_t *, size_t, bool, uint8_t, uint8_t *, uint32_t) { return -1; }
int Rc522::transceiveFrame(const uint8_t *, size_t, uint8_t *, size_t, uint32_t) { return -1; }
bool Pn5180::init() { return false; }
void Pn5180::end() {}
bool Pn5180::listTarget(NfcTarget &) { return false; }
int Pn5180::exchange(const uint8_t *, size_t, uint8_t *, size_t, uint32_t) { return -1; }
void Pn5180::release() {}
int Pn5180::transceiveRaw(const uint8_t *, size_t, uint8_t *, size_t, bool, uint8_t, uint8_t *, uint32_t) { return -1; }
int Pn5180::transceiveFrame(const uint8_t *, size_t, uint8_t *, size_t, uint32_t) { return -1; }
bool Pn5180::inventory15693(NfcTarget &) { return false; }
int Pn5180::exchange15693(const uint8_t *, size_t, uint8_t *, size_t) { return -1; }

FakeHost host;
Config config;
Backend backend;
Emulator emu(host, backend, config, nullptr);
int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                 \
        }                                                               \
    } while (0)

typedef std::vector<uint8_t> Bytes;

Bytes frame(uint8_t cmd, const Bytes &params) {
    Bytes body = {0xD4, cmd};
    body.insert(body.end(), params.begin(), params.end());
    Bytes f = {0x00, 0x00, 0xFF};
    size_t n = body.size();
    if (n > 255) {
        f.insert(f.end(), {0xFF, 0xFF, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF),
                           (uint8_t) - (int)((n >> 8) + (n & 0xFF))});
    } else {
        f.push_back(n);
        f.push_back((uint8_t) - (int)n);
    }
    uint8_t sum = 0;
    for (uint8_t b : body) {
        f.push_back(b);
        sum += b;
    }
    f.push_back((uint8_t)-sum);
    f.push_back(0);
    return f;
}

void runPolls(int n = 5) {
    for (int i = 0; i < n; i++) emu.poll();
}

// host.out as ACK + one response: 0 ok (data set), 1 no ACK, 2 no response,
// 3 syntax error frame, 4 malformed.
int parseOut(uint8_t cmd, Bytes &data, const Bytes &out) {
    static const Bytes ACK = {0, 0, 0xFF, 0, 0xFF, 0};
    if (out.size() < 6 || !std::equal(ACK.begin(), ACK.end(), out.begin())) return 1;
    Bytes r(out.begin() + 6, out.end());
    if (r.empty()) return 2;
    if (r.size() < 7 || r[0] || r[1] || r[2] != 0xFF) return 4;
    size_t len, header;
    if (r[3] == 0xFF && r[4] == 0xFF) {
        len = (r[5] << 8) | r[6];
        header = 8;
        if ((uint8_t)(r[5] + r[6] + r[7])) return 4;
    } else {
        len = r[3];
        header = 5;
        if ((uint8_t)(r[3] + r[4])) return 4;
    }
    if (r.size() != header + len + 2) return 4;
    uint8_t sum = 0;
    for (size_t i = 0; i <= len; i++) sum += r[header + i];
    if (sum) return 4;
    if (len == 1 && r[header] == 0x7F) return 3;
    if (r[header] != 0xD5 || r[header + 1] != cmd + 1) return 4;
    data.assign(r.begin() + header + 2, r.begin() + header + len);
    return 0;
}

int cmd(uint8_t c, const Bytes &params, Bytes &data, const Bytes &prefix = {}) {
    host.out.clear();
    Bytes f = prefix;
    Bytes fr = frame(c, params);
    f.insert(f.end(), fr.begin(), fr.end());
    host.in.insert(host.in.end(), f.begin(), f.end());
    runPolls();
    return parseOut(c, data, host.out);
}

#ifndef SIM
int main() {
    loadConfig(config);
    backend.begin(config);
    Bytes d;

    // Wakeup run + SAMConfiguration, firmware, libnfc's communication check
    CHECK(cmd(0x14, {0x01, 0x14, 0x01}, d, {0x55, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 0 && d.empty());
    CHECK(cmd(0x02, {}, d) == 0 && d == Bytes({0x32, 0x01, 0x06, 0x07}));
    CHECK(cmd(0x00, {0x00, 'l', 'i', 'b', 'n', 'f', 'c'}, d) == 0 && d == Bytes({0x00, 'l', 'i', 'b', 'n', 'f', 'c'}));
    // Short retries (as FF_NFC sets them), no card: quick empty answer
    CHECK(cmd(0x32, {0x05, 0xFF, 0x01, 0x02}, d) == 0);
    uint32_t t0 = fakeMillis;
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 && d == Bytes({0x00}));
    CHECK(fakeMillis - t0 < 20);

    // NTAG: select, write, read back
    mock.card = Card::Ntag;
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 &&
          d == Bytes({0x01, 0x01, 0x00, 0x44, 0x00, 0x07, 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}));
    CHECK(emu.cardSelected());
    CHECK(cmd(0x40, {0x01, 0xA2, 0x05, 1, 2, 3, 4}, d) == 0 && d == Bytes({0x00}));
    CHECK(cmd(0x40, {0x01, 0x30, 0x05}, d) == 0 && d.size() == 17 && d[0] == 0 && d[1] == 1 && d[4] == 4);
    CHECK(cmd(0x40, {0x02, 0x30, 0x05}, d) == 0 && d == Bytes({0x27}));          // wrong Tg
    // UID filter: with cascade tag (as libnfc sends it), and a wrong UID
    CHECK(cmd(0x4A, {0x01, 0x00, 0x88, 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, d) == 0 && d.size() == 13);
    CHECK(cmd(0x4A, {0x01, 0x00, 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x67}, d) == 0 && d == Bytes({0x00}));
    // InCommunicateThru: CRC off and 7 bits through the CIU registers, then back
    CHECK(cmd(0x08, {0x63, 0x02, 0x00, 0x63, 0x3D, 0x07}, d) == 0);
    CHECK(cmd(0x06, {0x63, 0x02, 0x63, 0x3D}, d) == 0 && d == Bytes({0x00, 0x07}));
    CHECK(cmd(0x42, {0x26}, d) == 0 && d == Bytes({0x00, 0x44, 0x00}) && !mock.lastCrc && mock.lastTxBits == 7);
    CHECK(cmd(0x08, {0x63, 0x02, 0x80, 0x63, 0x3D, 0x00}, d) == 0);
    CHECK(cmd(0x42, {0x26}, d) == 0 && mock.lastCrc && mock.lastTxBits == 0);
    CHECK(cmd(0x52, {0x00}, d) == 0 && d == Bytes({0x00}) && !emu.cardSelected());
    CHECK(cmd(0x40, {0x01, 0x30, 0x05}, d) == 0 && d == Bytes({0x27}));          // released

    // MIFARE Classic: wrong key, right key, two step write, read back
    mock.card = Card::Classic;
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 && d == Bytes({0x01, 0x01, 0x00, 0x04, 0x08, 0x04, 0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(cmd(0x40, {0x01, 0x60, 0x04, 0, 0, 0, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF}, d) == 0 && d == Bytes({0x14}));
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 && d[0] == 1);
    CHECK(cmd(0x40, {0x01, 0x60, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD, 0xBE, 0xEF}, d) == 0 &&
          d == Bytes({0x00}));
    Bytes w = {0x01, 0xA0, 0x05};
    for (int i = 0; i < 16; i++) w.push_back(0x40 + i);
    CHECK(cmd(0x40, w, d) == 0 && d == Bytes({0x00}));
    CHECK(cmd(0x40, {0x01, 0x30, 0x05}, d) == 0 && d.size() == 17 && d[1] == 0x40 && d[16] == 0x4F);

    // ISO-DEP: ATS in the target data; a 258 byte answer needs an extended frame
    mock.card = Card::IsoDep;
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 &&
          d == Bytes({0x01, 0x01, 0x00, 0x04, 0x20, 0x04, 1, 2, 3, 4, 0x05, 0x78, 0x80, 0x70, 0x02}));
    CHECK(cmd(0x40, {0x01, 0x00, 0xA4, 0x04, 0x00}, d) == 0 && d == Bytes({0x00, 0x00, 0xA4, 0x04, 0x00, 0x90, 0x00}));
    CHECK(cmd(0x40, {0x01, 0x00, 0xB0, 0x00, 0x00, 0x00}, d) == 0 && d.size() == 259 && d[0] == 0 && d[257] == 0x90);
    // NACK: the last answer again
    host.out.clear();
    host.in.insert(host.in.end(), {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00});
    runPolls();
    CHECK(host.out.size() == 8 + 2 + 259 + 2);
    // Automatic RATS off: no ATS; then on again
    CHECK(cmd(0x12, {0x04}, d) == 0 && !mock.autoIsoDep);
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 0 && d.size() == 10);
    CHECK(cmd(0x12, {0x14}, d) == 0 && mock.autoIsoDep);
    // InAutoPoll: type 0x10 (MIFARE) skips the ISO-DEP card, 0x20 finds it
    CHECK(cmd(0x60, {0x01, 0x01, 0x10}, d) == 0 && d == Bytes({0x00}));
    CHECK(cmd(0x60, {0x01, 0x01, 0x20}, d) == 0 && d.size() == 17 && d[0] == 1 && d[1] == 0x20 && d[2] == 14);

    // Endless retries, ended by the host's ACK: no answer, then normal again
    mock.card = Card::None;
    CHECK(cmd(0x32, {0x05, 0xFF, 0x01, 0xFF}, d) == 0);
    host.later.push_back({fakeMillis + 300, {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00}});
    t0 = fakeMillis;
    CHECK(cmd(0x4A, {0x01, 0x00}, d) == 2);
    CHECK(fakeMillis - t0 >= 300);
    CHECK(cmd(0x02, {}, d) == 0 && d.size() == 4);

    // A frame split across reads
    host.out.clear();
    Bytes f = frame(0x02, {});
    host.in.insert(host.in.end(), f.begin(), f.begin() + 4);
    emu.poll();
    CHECK(host.out.empty());
    host.in.insert(host.in.end(), f.begin() + 4, f.end());
    emu.poll();
    CHECK(parseOut(0x02, d, host.out) == 0);
    // Bad checksum: dropped without ACK; the next frame still works
    host.out.clear();
    f = frame(0x02, {});
    f[f.size() - 2] ^= 1;
    host.in.insert(host.in.end(), f.begin(), f.end());
    runPolls();
    CHECK(host.out.empty());
    CHECK(cmd(0x02, {}, d) == 0);
    // An extended frame from the host
    Bytes big = {0x00};
    for (int i = 0; i < 260; i++) big.push_back(i);
    CHECK(cmd(0x00, big, d) == 0 && d == big);
    // Not emulated: TgInitAsTarget
    CHECK(cmd(0x8C, {0x00}, d) == 3);

    // Bridge commands
    CHECK(cmd(0xF0, {}, d) == 0 && d.size() > 14 && memcmp(d.data(), "RFIDNFC2USB", 11) == 0 && d[11] == 1 && d[12] == 2);
    printf("info: %.*s\n", (int)d.size() - 14, (const char *)d.data() + 14);
    CHECK(cmd(0xF2, {}, d) == 0 && d == Bytes({0x00}));                          // no ISO 15693
    mock.iso15693 = true;
    CHECK(cmd(0xF4, {0x22, 0x20}, d) == 0 && d == Bytes({0x27}));                // inventory first
    CHECK(cmd(0xF2, {}, d) == 0 && d.size() == 9 && d[1] == 0xE0);
    CHECK(cmd(0xF4, {0x22, 0x20, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x04, 0xE0, 0x00}, d) == 0 &&
          d == Bytes({0x00, 0x00, 0x11, 0x22}));
    CHECK(cmd(0xF6, {}, d) == 0);
    printf("config: %.*s\n", (int)d.size(), (const char *)d.data());
    std::string bad = "{\"chip\":\"nope\"}";
    CHECK(cmd(0xF8, Bytes(bad.begin(), bad.end()), d) == 0 && d[0] == 1 && !ESP.restarted);
    printf("refused: %.*s\n", (int)d.size() - 1, (const char *)d.data() + 1);
    std::string good = "{\"chip\":\"pn5180\",\"busy\":12}";
    CHECK(cmd(0xF8, Bytes(good.begin(), good.end()), d) == 0 && d == Bytes({0x00}) && ESP.restarted);
    Config reloaded;
    loadConfig(reloaded);
    CHECK(reloaded.chip == ChipSetting::Pn5180 && reloaded.busy == 12);

    if (failures) printf("%d FAILED\n", failures);
    else printf("all passed\n");
    return failures != 0;
}
#endif
