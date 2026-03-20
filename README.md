# NABU BBS Terminal

An ANSI BBS telnet terminal for the [NABU Personal Computer](https://nabu.ca), written in C for the Z80.
Connect to ANSI BBS systems over the RetroNET Internet Adapter, with full CP437 character set support,
colour rendering, and ZModem file receive.

**Platform:** NABU PC (Z80, 64KB) · **Compiler:** z88dk + SDCC · **Binary type:** BIN_HOMEBREW

---

## Features

- **Telnet** — RFC 854 IAC negotiation: ECHO, SGA, subnegotiation (SB/SE) fully handled
- **ANSI/VT100 parser** — cursor movement, SGR colours, erase sequences, save/restore cursor
- **IBM CP437 font** — full 256-character set loaded at startup: printable ASCII, box-drawing,
  block graphics (░▒▓█) and extended characters for ANSI BBS art
- **Host preset menu** — five preset slots saved to `NBTERM.CFG` on the IA file store;
  editable in-session, survives restarts
- **ZModem receive** *(implemented, not yet working)* — ZModem receive state machine with
  CRC-16 and automatic CRC-32 fallback; currently timing out mid-transfer against real BBS
  systems. Under active investigation.
- **Reconnect loop** — returns to the main (preset) menu after each session.

---

## Building

Requires [z88dk](https://github.com/z88dk/z88dk) installed at `C:\z88dk\`.

**Windows (external terminal):**
```batch
build.bat
```

**Git Bash / VS Code terminal:**
```bash
Z88DK_DIR=/c/z88dk ZCCCFG=/c/z88dk/lib/config PATH=/c/z88dk/bin:$PATH \
  zcc +nabu -vn --list -m -create-app -compiler=sdcc -O3 --opt-code-speed \
  nterm.c -o "NABUTERM.nabu"
```

Output: `NABUTERM.nabu` (~30 KB)

---

## Usage

Load `NABUTERM.nabu` on the NABU. The preset menu appears on startup.

### Menu

| Key | Action |
|-----|--------|
| `1`–`5` | Connect to preset (opens edit dialog if slot is empty) |
| `E` | Edit a preset slot (hostname and port) |
| `D` | Delete a preset slot |
| `Q` | Quit — returns NABU to the boot loader |

### In session

| Key | Action |
|-----|--------|
| `CTRL-]` | Disconnect and return to menu |
| `CTRL-E` | Toggle local echo override |

ZModem transfers start automatically when the BBS initiates one. Received files are saved to
the IA file store under the filename provided by the sender - **Broken**.

---

## Files

| File | Description |
|------|-------------|
| `nterm.c` | Main entry point — init, reconnect loop, main telnet loop |
| `telnet.c/h` | RFC 854 IAC state machine, option negotiation, server echo flag |
| `ansi.c/h` | ANSI/VT100 escape sequence parser, ANSI colour sequence mapping, VDP output |
| `zmodem.c/h` | ZModem receive state machine, CRC-16/32, IA file store output |
| `menu.c/h` | Host preset menu, IA file-store persistence, line input |
| `cp437_patterns.h` | IBM CP437 8×8 font — ASCII and extended character bitmaps |
| `build.bat` | Windows build script |

---

## Development Phases

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | TCP skeleton — raw passthrough via RetroNET IA HCCA | ✅ Complete |
| 2 | Telnet IAC state machine — ECHO, SGA, subnegotiation | ✅ Complete |
| 3 | ANSI/VT100 escape sequence parser — cursor, colour, erase, SGR | ✅ Complete |
| 4 | Host preset menu — IA file-store persistence, reconnect loop, CTRL-E | ✅ Complete |
| 5 | ZModem receive — CRC-16, CRC-32 fallback, IA file output | ⚠️ In progress |
| 6 | CP437 font — full 256-character set, ANSI art rendering | ✅ Complete |
| 7 | Per-character ANSI colour (G2 mode, 40 columns) | 🔲 Planned |

---

## Known Limitations

- **Background colour** — TMS9918A TEXT80 mode uses a single global foreground/background
  register. Per-character background colouring is not possible in this mode; all text renders
  on a black background regardless of ANSI colour codes. A future G2-mode option would
  address this at the cost of dropping from 80 to 40 columns.

- **ZModem not yet working** — the receive state machine is implemented and partially functional
  (headers parse, files open, data sub-packets receive) but transfers fail mid-session, likely
  due to timing between the BBS sending ZDATA and ZEOF. Diagnostic VDP output is present in
  this build to aid debugging. ZModem should not be relied upon for file transfers at this stage.

---

## Background

Built to connect a real NABU Personal Computer to modern ANSI BBS systems running over TCP/IP
via the RetroNET Internet Adapter.

The CP437 font data is derived from the
[SSD1306Ascii project](https://github.com/greiman/SSD1306Ascii/blob/master/src/fonts/cp437font8x8.h)
by greiman (MIT licence). The ZModem implementation references
[zmp by mecparts](https://github.com/mecparts/zmp) and the dctelnet autostart detector technique
from [dctelnet by bruno-frederic](https://github.com/bruno-frederic/dctelnet).

---

## Licence

© Intangybles 2026. Released for use and adaptation with credit.
