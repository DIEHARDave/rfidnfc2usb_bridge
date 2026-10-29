// The emulator behind a pseudo terminal, with a mock NTAG215 holding an
// OpenSpool record, for host software (FF_NFC, bridgectl) to talk to.
#define SIM
#include "test.cpp"

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int master = -1;
static uint32_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pump() {
    fakeMillis = nowMs();
    uint8_t buf[512];
    ssize_t n;
    while ((n = read(master, buf, sizeof(buf))) > 0) host.in.insert(host.in.end(), buf, buf + n);
    if (!host.out.empty()) {
        ssize_t w = write(master, host.out.data(), host.out.size());
        if (w > 0) host.out.erase(host.out.begin(), host.out.begin() + w);
    }
}
void delay(uint32_t ms) {
    usleep(ms * 1000);
    pump();
}

int main(int argc, char **argv) {
    int slave;
    char name[128];
    if (openpty(&master, &slave, name, nullptr, nullptr) < 0) return 1;
    termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(master, F_SETFL, O_NONBLOCK);
    const char *link = argc > 1 ? argv[1] : "/tmp/bridge-pty";
    unlink(link);
    symlink(name, link);
    printf("pty %s -> %s\n", link, name);
    fflush(stdout);

    // NTAG215: CC, then an NDEF TLV with an OpenSpool JSON record.
    mock.card = Card::Ntag;
    uint8_t memory[540] = {0x04, 0x11, 0x22, 0xBB, 0x33, 0x44, 0x55, 0x66, 0x00, 0x48, 0x00, 0x00, 0xE1, 0x10, 0x3E, 0x00};
    const char *json = "{\"protocol\":\"openspool\",\"version\":\"1.0\",\"type\":\"PETG\",\"color_hex\":\"FF8800\",\"brand\":\"Bridge\",\"min_temp\":\"230\",\"max_temp\":\"250\"}";
    const char *mime = "application/json";
    size_t jl = strlen(json), ml = strlen(mime);
    size_t rec = 3 + ml + jl, pos = 16;
    memory[pos++] = 0x03;
    memory[pos++] = rec;
    memory[pos++] = 0xD2;                   // MB ME SR, TNF 2 (MIME)
    memory[pos++] = ml;
    memory[pos++] = jl;
    memcpy(memory + pos, mime, ml);
    pos += ml;
    memcpy(memory + pos, json, jl);
    pos += jl;
    memory[pos++] = 0xFE;
    for (int p = 0; p < 64; p++) memcpy(mock.mem[p], memory + p * 4, 4);

    loadConfig(config);
    backend.begin(config);
    pumpFn = pump;
    while (true) {
        pump();
        emu.poll();
        usleep(1000);
    }
}
