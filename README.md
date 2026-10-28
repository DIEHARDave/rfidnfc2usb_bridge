# esp32_rfidnfc2usb_bridge

ESP32 firmware that puts a **PN532**, **RC522** (MFRC522) or **PN5180** NFC
reader on USB. The computer sees one thing whichever chip is fitted: **a
PN532 on a serial port**. Software that already works with a PN532 over
UART works with the bridge unchanged: libnfc, nfcpy, zmod_nfc, the
Adafruit/Seeed PN532 libraries, and so on.

- **No drivers** on boards with native USB (ESP32-S2, S3, C3, C6): they
  appear as a standard USB CDC-ACM serial port, which Windows 10/11, Linux
  and macOS support out of the box.
- **PN532**: commands pass straight through to the chip, so everything the
  PN532 can do still works (type A/B, FeliCa, Jewel, target mode).
- **RC522 / PN5180**: the bridge emulates a PN532 as an ISO 14443A reader:
  NTAG/Ultralight, MIFARE Classic (authentication, reads, writes) and
  ISO-DEP/Type 4 cards.
- **PN5180 ISO 15693** (ICODE SLIX, OpenPrintTag): the PN532 protocol has
  no commands for these, so the bridge adds a few of its own (see
  [docs/protocol.md](docs/protocol.md)).

> **Status:** the PN532 on I2C works on hardware: on an ESP32-C3 on
> Windows (COM port, no driver), auto-detection found the PN532. zmod_nfc's
> PN532 reader then read an NTAG216 through the bridge (256 bytes in
> 0.8 s) and decoded its OpenSpool record. The RC522 and PN5180 emulation
> has not been run on hardware yet. It passes the native tests
> ([test/native](test/native)), and zmod_nfc read and decoded a mock tag
> through it over a pseudo-terminal. Its drivers come from
> [LaneTag](https://github.com/DIEHARDave/LaneTag), where
> they have not been run on hardware either. All seven board builds
> compile.
>
> **Seeed XIAO SAMD21:** the firmware runs on the board and answers the
> host as a PN532 (`bridgectl.py info` works on Windows, no driver), but it
> has **not been tested with any reader** wired to it yet.
>
> **Seeed XIAO RP2350:** the same: it flashes over USB, answers the host
> as a PN532 (`bridgectl.py info`), saves its settings across a restart and
> refuses pins its I2C, UART or SPI blocks cannot use, but it has **not been
> tested with any reader**.

## Drivers by board

| Board | Shows up as | Windows 10/11 | Linux | macOS |
|---|---|---|---|---|
| ESP32-S2, S3, C3, C6 (native USB port) | USB CDC-ACM (VID `303A`) | built-in (`usbser.sys`) | built-in (`cdc_acm`, `/dev/ttyACM*`) | built-in (`/dev/cu.usbmodem*`) |
| Classic ESP32 (CP210x or CH340 USB-UART chip) | that chip's serial port | usually installed by Windows Update | built-in (`/dev/ttyUSB*`) | depends on chip and macOS version; may need the vendor's driver |

Use a native-USB board if the host must work without any driver. On S3
boards with two USB connectors, use the one marked **USB** (wired to the
ESP32 itself), not **UART**.

The C3, C6 and S3 USB port also has a JTAG interface. Windows may list it
as a device with no driver; that interface is only for debugging, and the
serial port works without it.

## Hardware

Any of these boards, plus one reader module:

| Env | Board | Status LED |
|---|---|---|
| `esp32-c3` | ESP32-C3 SuperMini / DevKitM | GPIO 8 (active low) |
| `esp32-s3` | ESP32-S3 DevKitC and most S3 boards | none |
| `esp32-s2` | Wemos/Lolin S2 Mini | GPIO 15 |
| `esp32-c6` | ESP32-C6 DevKitC | none |
| `esp32dev` | classic ESP32 DevKitC, DOIT, NodeMCU-32S | GPIO 2 |
| `xiao-samd21` | Seeed XIAO SAMD21 (not an ESP32; untested with a reader) | yellow LED, pin 13 (active low) |
| `xiao-rp2350` | Seeed XIAO RP2350 (not an ESP32; untested) | yellow LED, GPIO 25 (active low) |

### Default pins

| Signal | C3 | S3 / S2 | C6 | classic |
|---|---|---|---|---|
| PN532 I2C SDA / SCL | 6 / 7 | 8 / 9 | 6 / 7 | 21 / 22 |
| PN532 UART RX / TX (ESP32 side) | 20 / 21 | 18 / 17 | 4 / 5 | 16 / 17 |
| SPI SCK / MISO / MOSI | 4 / 5 / 3 | 12 / 13 / 11 | 19 / 20 / 18 | 18 / 19 / 23 |
| SPI SS (RC522 SDA, PN5180 NSS) | 1 | 10 | 21 | 5 |
| RC522 / PN5180 RST | 0 | 14 | 22 | 4 |
| PN5180 BUSY | 10 | 15 (S3), 16 (S2) | 23 | 27 |

The I2C, UART and SPI pins do not overlap, so with the default `chip: auto`
the bridge finds whichever reader is wired. Every pin can be changed (see
[Settings](#settings)).

### Seeed XIAO SAMD21

**Not yet tested with a reader.** The SAMD21's I2C, UART and SPI sit on
fixed pins, so only `ss`, `rst`, `busy` and `led` can be changed:

| Signal | XIAO pin |
|---|---|
| PN532 I2C SDA / SCL | D4 / D5 |
| PN532 UART RX / TX (XIAO side) | D7 / D6 |
| SPI SCK / MISO / MOSI | D8 / D9 / D10 |
| SPI SS (RC522 SDA, PN5180 NSS) | D3 |
| RC522 / PN5180 RST | D2 |
| PN5180 BUSY | D1 |

- The XIAO's pins are **3.3 V only**; do not wire a reader's 5 V logic to them.
- The PN532 on I2C needs the pull-ups on its own board (every PN532 board
  has them). The SAMD21's internal pull-ups do not apply to the I2C pins,
  so with nothing wired the bus floats; the bridge checks for pull-ups and
  skips I2C when there are none.
- I2C reads stop at 255 bytes (the SAMD core's Wire limit), so PN532
  answers with more than 252 data bytes do not fit over I2C. UART has no
  such limit.

### Seeed XIAO RP2350

**Not yet tested with a reader.** The defaults use the same pads as the XIAO SAMD21; the
settings take GPIO numbers:

| Signal | XIAO pin | GPIO |
|---|---|---|
| PN532 I2C SDA / SCL (I2C1) | D4 / D5 | 6 / 7 |
| PN532 UART RX / TX (UART0, XIAO side) | D7 / D6 | 1 / 0 |
| SPI SCK / MISO / MOSI (SPI0) | D8 / D9 / D10 | 2 / 4 / 3 |
| SPI SS (RC522 SDA, PN5180 NSS) | D3 | 5 |
| RC522 / PN5180 RST | D2 | 28 |
| PN5180 BUSY | D1 | 27 |

- The pins are **3.3 V only**.
- The I2C, UART and SPI pins can move, but only to pins their hardware block
  can use (SDA/SCL pairs such as 4/5 or 6/7; UART TX/RX such as 0/1 or 4/5;
  SPI SCK/MOSI/MISO such as 2/3/4 or 10/11/12). The bridge refuses any
  other pins: the core would halt on them.

### Wiring notes

- **PN532** (red/blue "PN532 NFC V3" boards): I2C is switch 1 ON, 2 OFF; UART
  (HSU) is both OFF. Power from 3.3 V or 5 V (the board has a regulator).
  On UART, the PN532's TX goes to the ESP32's RX pin and RX to TX.
  If the PN532 is not found on I2C, try UART: the I2C pins double as the
  UART pins (SDA is TX, SCL is RX), so set both switches OFF, power the
  PN532 off and on (it reads its switches only at power-up), and set the
  UART RX/TX to the GPIOs SDA/SCL were on (or move the two wires to the
  default UART pins). With [LaneTag](https://github.com/DIEHARDave/LaneTag),
  two PN532 V3 boards held SCL low and were never found on I2C on an
  Ideaspark "ESP32 OLED 0.96" (classic ESP32), and both worked over UART.
  The PN532's SPI mode (switch 1 OFF, switch 2 ON) is not supported.
- **RC522**: SPI only, **3.3 V only**. `IRQ` is not used. RST can be tied to
  3.3 V (set `rst` to -1).
- **PN5180**: 5 V for the RF stage **and** 3.3 V for logic, both needed.
  BUSY must be wired. `IRQ`, `AUX`, `REQ` are not used.

## Build and flash

[PlatformIO](https://platformio.org/) (`pip install platformio`):

```sh
pio run -e esp32-c3 -t upload       # or esp32-s3, esp32-s2, esp32-c6, esp32dev
```

On the S2, hold **BOOT (0)** while plugging the board in the first time.
After that, `pio run -t upload` resets it itself. The C6 env uses the
pioarduino platform (Arduino core 3), which PlatformIO downloads the first
time.

One ESP32-C3 (revision v0.4, embedded XMC flash) would not take an upload
from the esptool 4.5.1 that platform 6.9.0 uses: "Unable to verify flash
chip connection" right after the stub started. Flashing without the stub
works, more slowly (about 25 s):

```sh
B=.pio/build/esp32-c3
python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32c3 --port COM7 \
  --no-stub write_flash 0x0 $B/bootloader.bin 0x8000 $B/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 $B/firmware.bin
```

**XIAO SAMD21:** `pio run -e xiao-samd21 -t upload`. If the upload fails
("No device found"), short RST to GND twice: an `Arduino` drive appears.
Copy a UF2 of the firmware onto it (the drive goes away as the board
restarts). PlatformIO builds only `firmware.bin`; convert it with
Microsoft's [uf2conv.py](https://github.com/microsoft/uf2/blob/master/utils/uf2conv.py)
(`uf2conv.py firmware.bin -c -b 0x2000 -f SAMD21 -o firmware.uf2`). If the board stops answering, unplug it and plug
it back in before trying again.

**XIAO RP2350:** `pio run -e xiao-rp2350 -t upload`. If no port shows up,
hold BOOT while plugging the board in: an `RP2350` drive appears, and
`.pio/build/xiao-rp2350/firmware.uf2` can be copied onto it.

The serial port carries only PN532 frames, so the firmware prints nothing
to it.

The PN532 module's range is short (2-4 cm, less through a spool or near
metal): a tag at the edge of its range shows up only now and then, and
reads stop part way. Lay tags flat on the antenna side of the board.

## Use

Point the software at the serial port as you would for a PN532 on UART at
115200 baud:

| Software | Setting |
|---|---|
| [zmod_nfc](https://github.com/DIEHARDave/zmod_nfc#usb-bridge-for-pn532-rc522-and-pn5180) (Zmod printers) | `reader: auto` or `reader: pn532` (finds `303A` and CP210x/CH340 ports itself), or `device: /dev/ttyACM0` |
| libnfc | `device.connstring = "pn532_uart:/dev/ttyACM0"` (Windows: `pn532_uart:COM6`) |
| nfcpy | `tty:ACM0:pn532` (Linux), `tty:usbmodem1101:pn532` (macOS), `com:COM6:pn532` (Windows) |
| Adafruit_PN532 / PN532 (Seeed) | the HSU/UART constructor on that port |

Keep DTR and RTS low, as PN532 software usually does. On classic ESP32
boards these lines drive reset and boot mode.

The LED lights while the host has a card selected and blinks slowly when
no reader chip answers.

### Tags on RC522 and PN5180

The emulation covers what PN532 software does with ISO 14443A tags:

- NTAG21x / Ultralight: read, write, `FAST_READ`, `GET_VERSION`, `PWD_AUTH`, ...
- MIFARE Classic 1K/4K: key A/B authentication, read, write, value blocks.
- ISO-DEP (Type 4, DESFire, bank cards): APDUs; the ATS is reported as a
  PN532 reports it.
- Raw frames through `InCommunicateThru`, including CRC off and 7-bit
  frames (for "magic" Classic cards).

Not available with these chips: target (card emulation) mode, peer-to-peer
(DEP), FeliCa, type B. The full command list is in
[docs/protocol.md](docs/protocol.md).

### ISO 15693 on the PN5180

`F2 Iso15693Inventory` and `F4 Iso15693Exchange` read and write ICODE
SLIX/SLIX2 tags (OpenPrintTag). PN532 software doesn't know these
commands, so it needs a few lines added to use them. zmod_nfc's PN532
reader doesn't use them yet.

## Settings

`tools/bridgectl.py` talks to the bridge over the same port (needs
`pip install pyserial`):

```sh
python tools/bridgectl.py ports                       # list USB serial ports
python tools/bridgectl.py --port COM6 info            # firmware, board, reader found
python tools/bridgectl.py --port COM6 config          # show settings
python tools/bridgectl.py --port COM6 config chip=rc522 ss=7 rst=-1
python tools/bridgectl.py --port COM6 reset-config    # board defaults
python tools/bridgectl.py --port COM6 scan            # print tags as they come
```

| Key | Values |
|---|---|
| `chip` | `auto` (PN532 I2C, PN532 UART, PN5180, RC522, in that order), `pn532_i2c`, `pn532_uart`, `rc522`, `pn5180` |
| `sda`, `scl`, `rx`, `tx`, `sck`, `miso`, `mosi`, `ss`, `rst`, `busy` | GPIO numbers, -1 for not wired |
| `led`, `led_active_low` | status LED, -1 for none |

Settings are kept in flash and the bridge restarts to apply them. On the
XIAO SAMD21 they live in the program flash, so a firmware upload resets
them to the defaults; on the XIAO RP2350 they live in the last flash
sector and survive an upload (`bridgectl.py reset-config` clears them).

## How it works

```
host (libnfc, zmod_nfc, ...)  --PN532 frames over USB CDC / UART-->  ESP32
                                                                      |
              Emulator (src/emulator.cpp): PN532 framing, ACK/NACK, abort
                |                                      |
         PN532: pass through                  RC522 / PN5180: emulate
         (src/pn532.cpp, I2C/UART)            (src/rc522.cpp, src/pn5180.cpp,
                                               src/iso_dep.cpp for ISO-DEP)
```

## Tests

`test/native` builds the emulator with g++ on Linux or WSL against a mock
card:

```sh
pio run -e esp32-c3          # once, to fetch ArduinoJson
cd test/native && make test  # emulator tests
make sim && ./sim &          # the emulator on /tmp/bridge-pty for host software
```

## License

GPL-3.0, as LaneTag, whose reader drivers this firmware uses.
