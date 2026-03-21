/*
 * NABU BBS Telnet Terminal
 * v1.01.01 -- Ctrl+T colour cycle (80-col); SYM key help overlay (both)
 *
 * Build G2 colour (stock):  zcc +nabu ...             nterm.c -o NABUTERM
 * Build 80-col (F18A):      zcc +nabu ... -DVDP_80COL nterm.c -o NABUTERM80
 *
 * NOTE: vdp_newLine() does NOT reset cursor column (known NABULIB behaviour).
 *       Always call vdp_setCursor2(0, vdp_cursor.y) after it -- use nl() here.
 */

/* -----------------------------------------------------------------------
 * Library configuration  (must come before the #includes)
 * --------------------------------------------------------------------- */
#define FONT_CP437
#define BIN_TYPE BIN_HOMEBREW
#define DISABLE_CURSOR          /* we drive the cursor ourselves */

/* Screen width -- used by ansi.c and menu.c.
 * Both builds use 80 as the logical column width:
 *   G2 build  : 80-column virtual buffer; 32 columns visible (viewport scroll)
 *   F18A build: 80 columns on screen directly */
#define SCREEN_COLS  80

/* G2 colour mode: active when NOT building for F18A. */
#ifndef VDP_80COL
#  define VDP_G2COL
#endif

#include "../NABULIB/NABU-LIB.h"
#include "../NABULIB/RetroNET-FileStore.h"
#include "cp437_patterns.h"

/* Sub-modules included directly -- single translation unit. */
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

static uint8_t  _conn_host[MENU_HOST_MAX + 1];
static uint8_t  _conn_host_len;
static uint16_t _conn_port;

/* -----------------------------------------------------------------------
 * zm_detect -- ZModem autostart detector (technique from dctelnet/Xfer.c)
 * Counts through the ZRQINIT sequence: * * 0x18 B 0 0
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
 * nl() -- newline + column reset for text-mode splash/message sections.
 * vdp_newLine() alone leaves cursor.x unchanged -- see NABULIB gotcha.
 * --------------------------------------------------------------------- */
static void nl(void)
{
    vdp_newLine();
    vdp_setCursor2(0, vdp_cursor.y);
}

/* -----------------------------------------------------------------------
 * _load_font -- load ASCII + CP437 extended patterns into VDP.
 * Must be called after every VDP mode initialisation.
 * --------------------------------------------------------------------- */
static void _load_font(void)
{
    uint8_t ci;
    vdp_loadASCIIFont(ASCII);
#ifdef VDP_G2COL
    /* With splitThirds=true the pattern generator is split into three 2048-byte
     * banks (rows 0-7, 8-15, 16-23).  vdp_loadASCIIFont only writes to bank 0;
     * we must copy the same data into banks 1 and 2 manually. */
    {
        const uint8_t *src;
        const uint8_t *end;
        src = ASCII;
        end = src + 768u;
        vdp_setWriteAddress(_vdpPatternGeneratorTableAddr + 2048u + 0x100u);
        do { IO_VDPDATA = *src; src++; } while (src != end);
        src = ASCII;
        vdp_setWriteAddress(_vdpPatternGeneratorTableAddr + 4096u + 0x100u);
        do { IO_VDPDATA = *src; src++; } while (src != end);
    }
#endif
    /* vdp_loadPatternToId already writes to all 3 banks when splitThirds=true. */
    for (ci = 0u; ci < 128u; ci++)
        vdp_loadPatternToId(0x80u + ci,
            (uint8_t *)CP437_EXT + (uint16_t)ci * 8u);
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
    bool     force_echo;

    /* -- Display init: text mode for the preset menu ------------------- */
#ifdef VDP_80COL
    vdp_initTextMode80(VDP_WHITE, VDP_BLACK, true);
#else
    vdp_initTextMode(VDP_WHITE, VDP_BLACK, true);
#endif
    _load_font();
    initNABULib();

    /* -- Outer reconnect loop ------------------------------------------ */
    while (1) {

        /* Show preset menu (text mode); blocks until user picks or quits */
        if (!menu_run(_conn_host, &_conn_host_len, &_conn_port))
            return;  /* Q pressed -- return to restart NABU */

        /* Connecting splash (text mode -- colours work here) */
        vdp_clearScreen();
        vdp_setCursor2(0, 0);
        vdp_setTextColor(VDP_CYAN, VDP_BLACK);
        vdp_print((uint8_t *)"NABU BBS Terminal  v1.01.01");
        nl();
        vdp_setTextColor(VDP_GRAY, VDP_BLACK);
        vdp_print((uint8_t *)"Connecting to: ");
        vdp_print(_conn_host);
        vdp_print((uint8_t *)" ...");
        nl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);

        handle = rn_TCPOpen(_conn_host_len, _conn_host, _conn_port, 0xFF);

        if (handle == 0xFF) {
            /* Connection failed -- stay in text mode, show error, loop */
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            vdp_print((uint8_t *)"Connection failed!");
            nl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            vdp_print((uint8_t *)"Press any key to return to menu...");
            while (!isKeyPressed()) ;
            getChar();
            continue;
        }

        /* -- Switch to terminal display mode --------------------------- */
#ifdef VDP_G2COL
        /* G2 colour mode: 32 visible cols, 80-col virtual buffer.
         * autoScroll=false: we manage vertical scrolling via the virtual
         * buffer so NABULIB does not interfere.
         * vdp_enableVDPReadyInt() enables the VDP VBlank interrupt so
         * vdp_waitVDPReadyInt() in ansi_render_viewport() can sync to
         * the beam and reduce screen tear during full redraws. */
        vdp_initG2Mode(VDP_BLACK, false, false, false, true);
        _load_font();
        vdp_enableVDPReadyInt();
#endif

        /* Reset sub-module state.  ansi_reset() sets G2 pattern colours
         * in G2 mode -- must be called AFTER vdp_initG2Mode(). */
        tn_init();
        ansi_reset();
        _zm        = 0;
        force_echo = false;

        /* Proactively request Suppress-Go-Ahead (full-duplex mode) */
        rn_TCPHandleWrite(handle, 0, 3, _sga_req);

        /* One-shot status hint -- scrolls away as BBS output flows */
        vdp_clearScreen();
        vdp_setCursor2(0, 0);
#ifdef VDP_80COL
        vdp_print((uint8_t *)"Connected.  ^] disc  ^E echo  ^T colour  SYM help");
#else
        vdp_print((uint8_t *)"Connected.  ^] disc  ^E echo  Arrows scroll  SYM help");
#endif
        nl();

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

                if (key == 0x1D)    /* CTRL-]  -- local disconnect */
                    break;

                if (key == 0x05) {  /* CTRL-E  -- toggle local echo override */
                    force_echo = !force_echo;
                    continue;
                }

#ifndef VDP_G2COL
                if (key == 0x14) { /* CTRL-T  -- cycle text colour (80-col) */
                    ansi_cycle_colour();
                    continue;
                }
#endif

                /* NABU special keys (0xE0-0xFF): never send to BBS.
                 * 0xE0-0xEA = key press codes; 0xF0-0xFA = key release
                 * codes sent automatically after each press (code | 0x10).
                 * Handle viewport scroll and SYM help here; drop the rest. */
                if (key >= 0xE0u) {
#ifdef VDP_G2COL
                    if      (key == 0xE1u) ansi_viewport_left();
                    else if (key == 0xE0u) ansi_viewport_right();
                    else if (key == 0xE5u) ansi_viewport_page_left();
                    else if (key == 0xE4u) ansi_viewport_page_right();
                    else if (key == 0xE8u) ansi_show_help();  /* SYM */
#else
                    if (key == 0xE8u) ansi_show_help();       /* SYM */
#endif
                    continue;  /* all 0xE0-0xFF consumed here, none to BBS */
                }

                if (key == 0x7F)    /* NABU backspace -> BS for server */
                    key = 0x08;

                if (key == 0x0A)    /* Enter -> CR for telnet NVT */
                    key = 0x0D;

                _tx[0] = key;
                rn_TCPHandleWrite(handle, 0, 1, _tx);

                if (!tn_server_echo || force_echo)
                    ansi_feed(key);
            }
        }

        rn_TCPHandleClose(handle);

        /* -- Switch back to text mode for menu ------------------------- */
#ifdef VDP_G2COL
        vdp_initTextMode(VDP_WHITE, VDP_BLACK, true);
        _load_font();
#endif

        vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
        nl();
        vdp_print((uint8_t *)"--- Disconnected ---");
        nl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);
        vdp_print((uint8_t *)"Press any key to return to menu...");
        while (!isKeyPressed()) ;
        getChar();
        /* Loop back to menu */
    }
}
