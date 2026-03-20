/*
 * NABU BBS Telnet Terminal
 * v1.00.05 — dual build: 80-col F18A (VDP_80COL) + 40-col stock TMS9918A
 *
 * Build 80-col (F18A):   zcc +nabu ... -DVDP_80COL nterm.c -o NABUTERM80
 * Build 40-col (stock):  zcc +nabu ...             nterm.c -o NABUTERM
 *
 * NOTE: vdp_newLine() does NOT reset cursor column (known NABULIB behaviour).
 *       Always call vdp_setCursor2(0, vdp_cursor.y) after it — use nl() here.
 */

/* -----------------------------------------------------------------------
 * Library configuration  (must come before the #includes)
 * --------------------------------------------------------------------- */
#define FONT_CP437
#define BIN_TYPE BIN_HOMEBREW
#define DISABLE_CURSOR          /* we drive the cursor ourselves */

/* Screen width — used by ansi.c and menu.c.
 * 80-col F18A build: pass -DVDP_80COL on the compiler command line.
 * 40-col stock TMS9918A build: omit the flag (default). */
#ifdef VDP_80COL
#  define SCREEN_COLS  80
#else
#  define SCREEN_COLS  40
#endif

#include "../NABULIB/NABU-LIB.h"
#include "../NABULIB/RetroNET-FileStore.h"
#include "cp437_patterns.h"

/* Sub-modules: included directly so they share the single translation unit.
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h inside these files. */
#include "telnet.c"
#include "ansi.c"
#include "menu.c"
#include "zmodem.c"

/* -----------------------------------------------------------------------
 * Buffers
 * --------------------------------------------------------------------- */
#define RX_BUF  128
static uint8_t _rx[RX_BUF];
static uint8_t _tx[1];

/* Active connection host/port — filled by menu_run() each session */
static uint8_t  _conn_host[MENU_HOST_MAX + 1];
static uint8_t  _conn_host_len;
static uint16_t _conn_port;

/* -----------------------------------------------------------------------
 * zm_detect — ZModem autostart detector (technique from dctelnet/Xfer.c)
 *
 * Counts through the ZRQINIT sequence: * * 0x18 B 0 0
 * Uses one byte of state. Returns true when the full sequence is matched.
 * --------------------------------------------------------------------- */
static uint8_t _zm = 0;

static bool zm_detect(uint8_t b)
{
    switch (b) {
    case '*':  _zm = (_zm < 2) ? (uint8_t)(_zm + 1) : 0; break;
    case 0x18: _zm = (_zm == 2) ? 3 : 0;                  break;
    case 'B':  _zm = (_zm == 3) ? 4 : 0;                  break;
    case '0':  _zm = (_zm == 4) ? 5 : 0;
               if (_zm == 5) { _zm = 0; return true; }
               break;
    default:   _zm = 0;                                    break;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * nl() — newline + column reset for use in splash/disconnect sections.
 * (vdp_newLine alone leaves cursor.x unchanged — see NABULIB gotcha.)
 * --------------------------------------------------------------------- */
static void nl(void)
{
    vdp_newLine();
    vdp_setCursor2(0, vdp_cursor.y);
}

/* Proactive IAC DO SGA sent immediately on every new connection */
static uint8_t _sga_req[3] = { 0xFF, 0xFD, 0x03 };

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
void main(void)
{
    uint8_t  handle;
    int32_t  avail;
    int32_t  got;
    uint16_t i;
    uint8_t  key;
    bool     force_echo;    /* CTRL-E override: echo locally even if server echoes */

    /* -- Display init (once) ------------------------------------------- */
    {
        uint8_t ci;
#ifdef VDP_80COL
        vdp_initTextMode80(VDP_WHITE, VDP_BLACK, true);  /* F18A 80-col   */
#else
        vdp_initTextMode(VDP_WHITE, VDP_BLACK, true);    /* stock 40-col  */
#endif
        vdp_loadASCIIFont(ASCII);
        for (ci = 0u; ci < 128u; ci++)
            vdp_loadPatternToId(0x80u + ci,
                (uint8_t *)CP437_EXT + (uint16_t)ci * 8u);
    }
    initNABULib();

    /* -- Outer reconnect loop ------------------------------------------ */
    while (1) {

        /* Show the preset menu; blocks until user picks a host or quits */
        if (!menu_run(_conn_host, &_conn_host_len, &_conn_port))
            return;  /* Q pressed — return from main() to restart NABU */

        /* Connecting splash */
        vdp_clearScreen();
        vdp_setCursor2(0, 0);   /* clearScreen does NOT reset cursor */
        vdp_setTextColor(VDP_CYAN, VDP_BLACK);
        vdp_print((uint8_t *)"NABU BBS Terminal  v1.00.05");
        nl();
        vdp_setTextColor(VDP_GRAY, VDP_BLACK);
        vdp_print((uint8_t *)"Connecting to: ");
        vdp_print(_conn_host);
        vdp_print((uint8_t *)" ...");
        nl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);

        /* Reset sub-module state for the new session */
        tn_init();
        ansi_reset();
        _zm       = 0;
        force_echo = false;

        handle = rn_TCPOpen(_conn_host_len, _conn_host, _conn_port, 0xFF);

        if (handle == 0xFF) {
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            vdp_print((uint8_t *)"Connection failed!");
            nl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            vdp_print((uint8_t *)"Press any key to return to menu...");
            while (!isKeyPressed())
                ;
            getChar();
            continue;   /* back to menu */
        }

        /* Proactively request Suppress-Go-Ahead (full-duplex mode) */
        rn_TCPHandleWrite(handle, 0, 3, _sga_req);

        /* One-shot status hint — scrolls away as BBS output flows */
        vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
#ifdef VDP_80COL
        vdp_print((uint8_t *)"Connected.  CTRL-] to disconnect.  CTRL-E toggles echo.");
#else
        vdp_print((uint8_t *)"Connected. CTRL-] disc  CTRL-E echo");
#endif
        nl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);

        /* -- Main telnet loop ------------------------------------------ */
        while (1) {

            /* 1. Read incoming data from the server */
            avail = rn_TCPHandleSize(handle);
            if (avail == -1)
                break;  /* server closed the connection */

            if (avail > 0) {
                if (avail > RX_BUF)
                    avail = RX_BUF;

                got = rn_TCPHandleRead(handle, _rx, 0, (uint16_t)avail);
                if (got == -1)
                    break;

                for (i = 0; i < (uint16_t)got; i++) {
                    if (tn_feed(_rx[i], handle)) {
                        if (zm_detect(_rx[i])) {
                            /* Pass bytes already in _rx after the detection
                             * point -- they may contain ZSINIT/ZFILE frames
                             * that rn_TCPHandleRead() already consumed from
                             * the IA buffer and would otherwise be lost. */
                            {
                                uint8_t rem = (uint8_t)(
                                    (int32_t)got - (int32_t)(i + 1));
                                zmodem_receive(handle,
                                               _rx + (uint8_t)(i + 1u),
                                               rem);
                            }
                            break;
                        } else {
                            ansi_feed(_rx[i]);
                        }
                    }
                }
            }

            /* 2. Handle keyboard input */
            if (isKeyPressed()) {
                key = getChar();

                if (key == 0x1D)    /* CTRL-]  — local disconnect */
                    break;

                if (key == 0x05) {  /* CTRL-E  — toggle local echo override */
                    force_echo = !force_echo;
                    continue;
                }

                if (key == 0x7F)    /* NABU backspace → BS for server */
                    key = 0x08;

                if (key == 0x0A)    /* Enter → CR for telnet NVT */
                    key = 0x0D;

                _tx[0] = key;
                rn_TCPHandleWrite(handle, 0, 1, _tx);

                /* Echo locally when server is not echoing, or override active */
                if (!tn_server_echo || force_echo)
                    ansi_feed(key);
            }
        }

        /* -- Disconnect ------------------------------------------------ */
        rn_TCPHandleClose(handle);
        vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
        nl();
        vdp_print((uint8_t *)"--- Disconnected ---");
        nl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);
        vdp_print((uint8_t *)"Press any key to return to menu...");
        while (!isKeyPressed())
            ;
        getChar();
        /* Loop back to menu */
    }
}
