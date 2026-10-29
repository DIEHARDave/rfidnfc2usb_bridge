# Host protocol

The bridge is a PN532 on a serial port. The host sends PN532 frames as it
would to a PN532 module in HSU (UART) mode, as described in the NXP *PN532
User Manual* (UM0701-02), and gets the PN532's answers. Any PN532 software
that can use a serial port works unchanged: libnfc (`pn532_uart:` driver),
nfcpy (`tty:` path), FF_NFC, the Adafruit and Seeed PN532 libraries.

- **Link.** USB CDC-ACM on boards with native USB (any baud rate setting
  works). On classic ESP32 boards, the board's USB-UART chip at
  115200 baud 8N1. DTR and RTS should stay low, as with a PN532 module:
  on classic boards they drive the ESP32's reset and boot pins.
- **Frames.** Normal and extended information frames, ACK, NACK and the
  syntax error frame, as in UM0701-02 section 6.2. The wakeup run
  (`55 55 00 00 ...`) is accepted and ignored: the bridge never sleeps.
  A frame with a bad checksum is dropped with no ACK, as on the PN532.
- **Abort.** Any byte from the host while a command runs (normally an ACK
  frame) stops it with no answer. This ends an `InListPassiveTarget` or
  `InAutoPoll` that is waiting for a card.

## PN532 commands

With a **PN532** behind the bridge, every command is passed through to the
chip, except:

| Command | What the bridge does |
|---|---|
| `InListPassiveTarget` (4A), `InAutoPoll` (60) | Retries the activation itself, so the host can abort it. |
| `RFConfiguration` (32) item 5 | Remembers `MxRtyPassiveActivation` for the above; the chip gets 1. |
| `SetSerialBaudRate` (10) | Changes the host link (classic boards); the chip keeps its own. |
| `PowerDown` (16) | Releases the card and answers OK; nothing sleeps. |

With an **RC522** or a **PN5180**, the bridge emulates a PN532 v1.6 as an
ISO 14443A initiator at 106 kbps:

| Command | Emulation |
|---|---|
| `Diagnose` (00) | Test 0 (communication line) echoes; the other tests report OK. |
| `GetFirmwareVersion` (02) | `32 01 06 07`, like a PN532 v1.6. |
| `GetGeneralStatus` (04) | Field, one target when one is selected, SAM status 0. |
| `ReadRegister` (06), `WriteRegister` (08) | A register store that does not reach the chip, except: `CIU_TxMode` bit 7 (CRC on/off) and `CIU_BitFraming` TxLastBits apply to `InCommunicateThru`; `CIU_Control` RxLastBits reports the bits of its last answer. |
| `ReadGPIO` (0C), `WriteGPIO` (0E), `SAMConfiguration` (14) | Answered, no effect. |
| `SetParameters` (12) | `fAutomaticRATS` (bit 4) switches ISO-DEP activation on and off. |
| `RFConfiguration` (32) | Item 1 with the field off releases the card; item 5 sets the activation retries; the rest are accepted. |
| `InListPassiveTarget` (4A) | `BrTy` 0 (106 kbps type A): one card, with `InitiatorData` (a UID, with or without cascade tags) selecting a given one. ISO-DEP cards get RATS and their ATS is in the answer, as on the PN532. Other `BrTy` find nothing. |
| `InDataExchange` (40) | MIFARE Classic authentication (`60`/`61` + block + key + UID), reads, writes (`A0`, two steps), value commands (`C0`-`C2`, `B0`), Ultralight/NTAG commands (`30`, `A2`, `3A`, `60` GET_VERSION, `1B`, ...), and APDUs to ISO-DEP cards (chained both ways). `MI` chaining from the host is not supported. |
| `InCommunicateThru` (42) | One raw frame, with or without CRC per `CIU_TxMode`, short frames per `CIU_BitFraming` (for example the 7 bit `40` of "magic" MIFARE cards). |
| `InDeselect` (44), `InRelease` (52), `InSelect` (54) | HLTA or S(DESELECT); `InSelect` wakes the same card again. |
| `InPSL` (4E) | OK (106 kbps only). |
| `InAutoPoll` (60) | Types `00`, `10`, `20` (type A). |
| `PowerDown` (16), `SetSerialBaudRate` (10) | As with a PN532 above. |

Target mode (`Tg*`), ISO 18092 / DEP (`InJumpForDEP`, `InATR`, ...),
FeliCa and type B need a PN532: the emulation answers them with a syntax
error.

Status bytes in emulated answers: `00` OK, `01` no answer from the card,
`14` MIFARE authentication refused, `27` no card selected.

## Bridge commands

Frames like any PN532 command (`D4 <cmd> <params>`, answer `D5 <cmd+1>
<data>`). A real PN532 answers them with a syntax error frame, so
`BridgeInfo` tells a bridge from a PN532.

### F0 BridgeInfo

No parameters. Answer:

| Bytes | |
|---|---|
| 11 | `RFIDNFC2USB` (ASCII) |
| 1 | protocol version, 1 |
| 1 | reader chip: 0 none answering, 1 PN532, 2 RC522, 3 PN5180 |
| 1 | flags: bit 0, ISO 15693 commands work (PN5180) |
| rest | ASCII `key=value` pairs separated by `;`: `firmware`, `board` (the ESP32 type), `reader` (chip, firmware, bus), `setting` (the chip setting) |

### F2 Iso15693Inventory (PN5180 only)

No parameters. One inventory with one slot at high data rate. Answer:
`00` (no card), or `01` and the 8 byte UID, most significant byte (`E0`)
first. It releases any ISO 14443A card: the PN5180 has one field mode at a
time.

### F4 Iso15693Exchange (PN5180 only)

Parameters: an ISO 15693 request from its flags byte on, without CRC (for
example `22 23 <uid, LSB first> <block> <count>` for READ MULTIPLE BLOCKS
addressed). The bridge adds and checks the CRC. Answer: a status byte (`00`
OK, `01` no answer, `27` no inventory first) and the card's answer from its
flags byte on.

### F6 GetConfig

No parameters. Answer: the settings as JSON text, for example

```json
{"chip":"auto","sda":6,"scl":7,"rx":20,"tx":21,"sck":4,"miso":5,"mosi":3,
 "ss":1,"rst":0,"busy":10,"led":8,"led_active_low":true}
```

### F8 SetConfig

Parameters: a JSON object with the keys to change (the others stay), or
`{"reset": true}` for the board's defaults. Answer: `00` and the bridge
restarts with the new settings once the answer is sent; or `01` and the
reason as text, and nothing changes.

| Key | |
|---|---|
| `chip` | `auto` (probe PN532 on I2C, PN532 on UART, PN5180, RC522), `pn532_i2c`, `pn532_uart`, `rc522`, `pn5180` |
| `sda`, `scl` | PN532 I2C |
| `rx`, `tx` | PN532 UART (ESP32 RX and TX) |
| `sck`, `miso`, `mosi`, `ss` | RC522 / PN5180 SPI (`ss`: RC522 SDA, PN5180 NSS) |
| `rst` | RC522 / PN5180 reset, -1 when not wired |
| `busy` | PN5180 BUSY |
| `led`, `led_active_low` | status LED, -1 for none |

### FA Restart

No parameters. Answers, then restarts.
