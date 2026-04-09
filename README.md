# NABU BBS Terminal

An ANSI BBS telnet terminal for the [NABU Personal Computer](https://nabu.ca), written in C for the Z80.
Connect to ANSI BBS systems over the RetroNET Internet Adapter, with full CP437 character set support,
per-character ANSI colour, and ZModem file receive.

**Platform:** NABU PC (Z80, 64KB) · **Compiler:** z88dk + SDCC · **Binary type:** BIN_HOMEBREW

---

> Two binaries are produced from a single source. Choose the right one for your hardware:
>
> | Binary | Mode | Hardware |
> |--------|------|----------|
> | `NABUTERM.nabu` | 40-column | Stock NABU -- TMS9918A (default) |
> | `NABUTERM80.nabu` | 80-column | F18A or similar upgrade required |

---

## Features

- **Telnet** -- RFC 854 IAC negotiation: ECHO, SGA, subnegotiation (SB/SE) fully handled
- **ANSI/VT100 parser** -- cursor movement, SGR colours, erase sequences, save/restore cursor
- **IBM CP437 font** -- full 256-character set loaded at startup: printable ASCII, box-drawing,
  block graphics (░▒▓█) and extended characters for ANSI BBS art
- **Per-character ANSI colour** -- Graphics II mode on the 40-column build gives true
  per-character foreground and background colour on stock TMS9918A hardware
- **80-column virtual buffer** -- BBS content rendered into an 80-column virtual buffer with a
  scrollable 32-column viewport (40-column build); arrow keys pan left and right
- **Host preset menu** -- five preset slots saved to `NBTERM.CFG` on the IA file store;
  editable in-session, survives restarts
- **ZModem receive** -- working receive implementation with CRC-16; tested against AmiExpress
  and Mystic BBS systems on real NABU hardware. Received files written to the IA file store.
- **Reconnect loop** -- returns to the preset menu after each session ends

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

Both targets are built from `nterm.c` alone -- sub-modules are pulled in via `#include`.

Output sizes: `NABUTERM.nabu` ~40 KB · `NABUTERM80.nabu` ~34 KB

---

## Usage

Load `NABUTERM.nabu` (or `NABUTERM80.nabu`) on the NABU. The preset menu appears on startup.

### Menu

| Key | Action |
|-----|--------|
| `1`--`5` | Connect to preset (opens edit dialog if slot is empty) |
| `E` | Edit a preset slot (hostname and port) |
| `D` | Delete a preset slot |
| `Q` | Quit -- returns NABU to the boot loader |

### In session

| Key | Action |
|-----|--------|
| `CTRL-]` | Disconnect and return to menu |
| `CTRL-E` | Toggle local echo override |
| `CTRL-T` | Cycle text colour (80-column build only) |
| `SYM` | Show key reference overlay |
| `Left` / `Right` | Scroll viewport 1 column (40-column build only) |
| `Page Left` / `Page Right` | Scroll viewport 8 columns (40-column build only) |

ZModem transfers start automatically when the BBS initiates one. The filename and size are
shown on screen; received files are saved to the IA file store under the sender's filename.

---

## Files

| File | Description |
|------|-------------|
| `nterm.c` | Main entry point -- init, reconnect loop, main telnet/ZModem loop |
| `telnet.c/h` | RFC 854 IAC state machine, option negotiation, server echo flag |
| `ansi.c/h` | ANSI/VT100 escape sequence parser, colour mapping, VDP output, help overlay |
| `zmodem.c/h` | ZModem receive state machine, CRC-16/32, IA file store output |
| `menu.c/h` | Host preset menu, IA file-store persistence, line input |
| `cp437_patterns.h` | IBM CP437 8x8 font -- ASCII and extended character bitmaps |
| `manual/NABUTERM.md` | User manual (Markdown) |
| `manual/NABUTERM.txt` | User manual (plain text) |
| `build.bat` | Windows build script |

---

## Development Phases

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | TCP skeleton -- raw passthrough via RetroNET IA HCCA | Complete |
| 2 | Telnet IAC state machine -- ECHO, SGA, subnegotiation | Complete |
| 3 | ANSI/VT100 escape sequence parser -- cursor, colour, erase, SGR | Complete |
| 4 | Host preset menu -- IA file-store persistence, reconnect loop | Complete |
| 5 | ZModem receive -- CRC-16, IA file output | Complete |
| 6 | CP437 font -- full 256-character set, ANSI art rendering | Complete |
| 7 | Per-character ANSI colour -- Graphics II mode, 40 columns | Complete |
| 8 | Dual build -- 40-col stock TMS9918A + 80-col F18A or similar | Complete |
| 9 | ZModem AmiExpress compatibility -- telnet IAC fix, display | Complete |
| 10 | 80-col virtual buffer, viewport scroll, ZModem G2 fix -- v1.02.17 | Complete |

---

## Known Limitations

- **80-column mode requires F18A or similar upgrade** -- `NABUTERM80.nabu` uses F18A extensions
  and will not run on a stock TMS9918A. Use `NABUTERM.nabu` for stock hardware.

- **80-column background colour** -- F18A (and similar) TEXT80 mode uses a single global
  foreground/background register. Per-character background colouring is not possible in this
  mode; all text renders on a black background regardless of ANSI colour codes.
  The 40-column build does not have this limitation.

- **ZModem send not implemented** -- receive only. The NABU cannot initiate a ZModem upload.

---

## Background

Built to connect a real NABU Personal Computer to modern ANSI BBS systems running over TCP/IP
via the RetroNET Internet Adapter.

The CP437 font data is derived from the
[SSD1306Ascii project](https://github.com/greiman/SSD1306Ascii/blob/master/src/fonts/cp437font8x8.h)
by greiman (MIT licence). The ZModem implementation references
[zmp by Wayne Warthen](https://github.com/mecparts/zmp) (a CP/M ZModem for Z80) and uses the
ZModem autostart detector technique from [dctelnet](https://github.com/bruno-frederic/dctelnet)
by Bruno Frederic. AmiExpress sender behaviour was analysed from the
[AmiExpress source](https://github.com/dmcoles/AmiExpress) by dmcoles.

---

## Licence

© Intangybles 2026. Released for use and adaptation with credit.
