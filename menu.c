/*
 * menu.c -- Host/preset selection menu with IA file-store persistence
 *
 * Included into nterm.c via  #include "menu.c"
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h here; included by nterm.c.
 *
 * Presets are stored in NBTERM.CFG on the IA file store, one per line:
 *   hostname:port\n
 * Empty slots are stored as a bare newline.  Five lines, always.
 *
 * Controls:
 *   [1-5]  Connect to preset (prompts edit if slot is empty)
 *   [E]    Edit a preset slot
 *   [D]    Delete a preset slot
 */

#include "menu.h"
#include <stdbool.h>

/* ----------------------------------------------------------------------- */
#define _NPRESETS  5

typedef struct {
    uint8_t  host[MENU_HOST_MAX + 1];  /* null-terminated; host[0]==0 => empty */
    uint16_t port;
} _Preset;

static _Preset _presets[_NPRESETS];

/* Config file name on the IA file store */
static uint8_t _cfg[] = "NBTERM.CFG";
#define _CFG_LEN ((uint8_t)(sizeof(_cfg) - 1))

/* Default first preset when no config file exists */
static uint8_t _dflt_host[] = "bbs.retrobattlestations.com";

/* -----------------------------------------------------------------------
 * _u16tostr — write uint16_t as ASCII decimal into buf[]; return char count.
 * Caller must null-terminate if needed.
 * --------------------------------------------------------------------- */
static uint8_t _u16tostr(uint8_t *buf, uint16_t v)
{
    uint8_t tmp[5];
    uint8_t n, i;
    n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) {
        tmp[n++] = (uint8_t)('0' + v % 10);
        v = (uint16_t)(v / 10);
    }
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

/* -----------------------------------------------------------------------
 * _load_presets — read NBTERM.CFG from the IA file store.
 * Falls back to a single default preset when the file doesn't exist.
 * --------------------------------------------------------------------- */
static void _load_presets(void)
{
    uint8_t  fh;
    uint8_t  line[80];
    uint8_t  i, j;
    uint16_t cnt, linelen;
    uint8_t  *p;
    uint16_t port;

    for (i = 0; i < _NPRESETS; i++) {
        _presets[i].host[0] = 0;
        _presets[i].port = 23;
    }

    fh = rn_fileOpen(_CFG_LEN, _cfg, OPEN_FILE_FLAG_READONLY, 0xFF);
    if (fh == 0xFF) {
        /* No config yet — populate slot 1 with the default BBS */
        for (j = 0; _dflt_host[j]; j++) _presets[0].host[j] = _dflt_host[j];
        _presets[0].host[j] = 0;
        _presets[0].port = 23;
        return;
    }

    cnt = rn_fileHandleLineCount(fh);

    for (i = 0; i < _NPRESETS; i++) {
        if ((uint16_t)i >= cnt) break;
        /* rn_fileHandleGetLine does NOT null-terminate — do it ourselves */
        linelen = rn_fileHandleGetLine(fh, (uint16_t)i, line);
        if (linelen < (uint16_t)sizeof(line)) line[linelen] = 0;
        else line[sizeof(line) - 1] = 0;

        /* Parse "hostname:port" */
        p = line;
        j = 0;
        while (*p && *p != ':' && *p != '\r' && *p != '\n' && j < MENU_HOST_MAX) {
            _presets[i].host[j++] = *p++;
        }
        _presets[i].host[j] = 0;

        port = 0;
        if (*p == ':') {
            p++;
            while (*p >= '0' && *p <= '9') {
                port = (uint16_t)(port * 10u + (uint16_t)(*p - '0'));
                p++;
            }
        }
        if (port > 0) _presets[i].port = port;
    }

    rn_fileHandleClose(fh);
}

/* -----------------------------------------------------------------------
 * _save_presets — write all five presets to NBTERM.CFG.
 * --------------------------------------------------------------------- */
static void _save_presets(void)
{
    uint8_t fh;
    uint8_t buf[80];
    uint8_t i, j, len, plen;

    fh = rn_fileOpen(_CFG_LEN, _cfg, OPEN_FILE_FLAG_READWRITE, 0xFF);
    if (fh == 0xFF) return;

    rn_fileHandleEmptyFile(fh);

    for (i = 0; i < _NPRESETS; i++) {
        len = 0;
        if (_presets[i].host[0]) {
            for (j = 0; _presets[i].host[j] && len < 70; j++)
                buf[len++] = _presets[i].host[j];
            buf[len++] = ':';
            plen = _u16tostr(buf + len, _presets[i].port);
            len = (uint8_t)(len + plen);
        }
        buf[len++] = '\n';
        rn_fileHandleAppend(fh, 0, len, buf);
    }

    rn_fileHandleClose(fh);
}

/* -----------------------------------------------------------------------
 * _mn — newline + column reset (mirrors nl() in nterm.c).
 * vdp_newLine() does NOT reset cursor.x — always fix it after.
 * --------------------------------------------------------------------- */
static void _mn(void)
{
    vdp_newLine();
    vdp_setCursor2(0, vdp_cursor.y);
}

/* -----------------------------------------------------------------------
 * _draw_menu — redraw the full preset list on screen.
 * --------------------------------------------------------------------- */
static void _draw_menu(void)
{
    uint8_t port_buf[6];
    uint8_t plen;
    uint8_t i;

    vdp_clearScreen();
    vdp_setCursor2(0, 0);   /* clearScreen does NOT reset cursor */

    vdp_setTextColor(VDP_CYAN, VDP_BLACK);
    vdp_print((uint8_t *)"NABU BBS Terminal  v1.00.05");
    _mn();
    vdp_setTextColor(VDP_GRAY, VDP_BLACK);
#ifdef VDP_80COL
    vdp_print((uint8_t *)"------------------------------------------------------------");
#else
    vdp_print((uint8_t *)"--------------------------------------");
#endif
    _mn();
    _mn();

    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
    vdp_print((uint8_t *)"  Presets:");
    _mn();
    _mn();

    for (i = 0; i < _NPRESETS; i++) {
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);
        vdp_write((uint8_t)('1' + i));
        vdp_print((uint8_t *)".  ");
        if (_presets[i].host[0]) {
            vdp_setTextColor(VDP_LIGHT_GREEN, VDP_BLACK);
            vdp_print(_presets[i].host);
            vdp_setTextColor(VDP_GRAY, VDP_BLACK);
            vdp_write(':');
            plen = _u16tostr(port_buf, _presets[i].port);
            port_buf[plen] = 0;
            vdp_print(port_buf);
        } else {
            vdp_setTextColor(VDP_GRAY, VDP_BLACK);
            vdp_print((uint8_t *)"(empty)");
        }
        _mn();
    }

    _mn();
    vdp_setTextColor(VDP_GRAY, VDP_BLACK);
#ifdef VDP_80COL
    vdp_print((uint8_t *)"------------------------------------------------------------");
#else
    vdp_print((uint8_t *)"--------------------------------------");
#endif
    _mn();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
#ifdef VDP_80COL
    vdp_print((uint8_t *)"  [1-5] Connect   [E] Edit   [D] Delete   [Q] Quit");
#else
    vdp_print((uint8_t *)"[1-5]Connect [E]Edit [D]Del [Q]Quit");
#endif
    _mn();
}

/* -----------------------------------------------------------------------
 * _input_line — read a line from keyboard into buf (max maxlen chars).
 *
 * Shows a static underscore as a cursor indicator.  Handles printable
 * ASCII, backspace (0x08 / 0x7F), Enter (commit), and ESC (cancel).
 *
 * Invariant: vdp cursor is always parked ON the underscore character.
 *
 * Returns true on Enter, false on ESC.
 * Caller positions cursor to the start of the input field before calling.
 * --------------------------------------------------------------------- */
static bool _input_line(uint8_t *buf, uint8_t maxlen)
{
    uint8_t len;
    uint8_t key;
    uint8_t cx;

    len = 0;
    buf[0] = 0;

    /* Show initial underscore cursor */
    vdp_setTextColor(VDP_LIGHT_YELLOW, VDP_BLACK);
    vdp_write('_');
    vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);

    while (1) {
        while (!isKeyPressed())
            ;
        key = getChar();

        if (key == 0x0D || key == 0x0A) {
            /* Enter — erase underscore and commit */
            vdp_write(' ');
            vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
            buf[len] = 0;
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            return true;
        }

        if (key == 0x1B) {
            /* ESC — cancel */
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            return false;
        }

        if ((key == 0x08 || key == 0x7F) && len > 0) {
            /* Backspace: move left, write underscore over previous char,
             * write space over old underscore, park cursor on underscore */
            len--;
            buf[len] = 0;
            cx = vdp_cursor.x;
            vdp_setCursor2((uint8_t)(cx - 1), vdp_cursor.y);
            vdp_write('_');
            vdp_write(' ');
            vdp_setCursor2((uint8_t)(cx - 1), vdp_cursor.y);
            continue;
        }

        if (key >= 0x20 && key < 0x7F && len < maxlen) {
            /* Printable: write char over underscore, then new underscore */
            buf[len++] = key;
            buf[len]   = 0;
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            vdp_write(key);
            if (len < maxlen) {
                vdp_setTextColor(VDP_LIGHT_YELLOW, VDP_BLACK);
                vdp_write('_');
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * _edit_preset — prompt user to enter host and port for slot idx.
 * Edit area uses rows 20-23 (below the preset list).
 * --------------------------------------------------------------------- */
static void _edit_preset(uint8_t idx)
{
    uint8_t  new_host[MENU_HOST_MAX + 1];
    uint8_t  port_buf[6];
    uint8_t  hint_buf[6];
    uint8_t  hint_len;
    uint8_t  j;
    uint16_t port;
    bool     ok;

    vdp_setCursor2(0, 20);
    vdp_clearRows(20, 23);
    vdp_setCursor2(0, 20);

    vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
    vdp_print((uint8_t *)"  Edit preset ");
    vdp_write((uint8_t)('1' + idx));
    vdp_print((uint8_t *)"  (blank = cancel)");
    _mn();

    /* --- Hostname --- */
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
    vdp_print((uint8_t *)"  Host: ");
    new_host[0] = 0;
    ok = _input_line(new_host, MENU_HOST_MAX);

    if (!ok || new_host[0] == 0) {
        vdp_clearRows(20, 23);
        return;
    }

    for (j = 0; new_host[j]; j++) _presets[idx].host[j] = new_host[j];
    _presets[idx].host[j] = 0;

    /* --- Port (show current as hint) --- */
    _mn();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
    vdp_print((uint8_t *)"  Port (");
    hint_len = _u16tostr(hint_buf, _presets[idx].port);
    hint_buf[hint_len] = 0;
    vdp_print(hint_buf);
    vdp_print((uint8_t *)"): ");

    port_buf[0] = 0;
    ok = _input_line(port_buf, 5);

    if (ok && port_buf[0]) {
        port = 0;
        for (j = 0; port_buf[j] >= '0' && port_buf[j] <= '9'; j++)
            port = (uint16_t)(port * 10u + (uint16_t)(port_buf[j] - '0'));
        if (port > 0) _presets[idx].port = port;
    }
    /* else: blank or cancel — keep existing port */

    _save_presets();
    vdp_clearRows(20, 23);
}

/* -----------------------------------------------------------------------
 * menu_run — public entry point.
 * Draws the menu and blocks until the user selects a non-empty preset.
 * --------------------------------------------------------------------- */
bool menu_run(uint8_t *host_out, uint8_t *host_len_out, uint16_t *port_out)
{
    uint8_t key;
    uint8_t idx;
    uint8_t j;

    _load_presets();
    _draw_menu();

    while (1) {
        while (!isKeyPressed())
            ;
        key = getChar();

        /* [1-5] — connect or edit-then-connect */
        if (key >= '1' && key <= '5') {
            idx = (uint8_t)(key - '1');
            if (_presets[idx].host[0] == 0) {
                /* Empty slot — offer to fill it first */
                _edit_preset(idx);
                _draw_menu();
                continue;
            }
            /* Copy host and port to caller's buffers */
            for (j = 0; _presets[idx].host[j]; j++)
                host_out[j] = _presets[idx].host[j];
            host_out[j]   = 0;
            *host_len_out = j;
            *port_out     = _presets[idx].port;
            return true;
        }

        /* [E] / [e] — edit a preset */
        if (key == 'E' || key == 'e') {
            vdp_setCursor2(0, 20);
            vdp_clearRows(20, 23);
            vdp_setCursor2(0, 20);
            vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
            vdp_print((uint8_t *)"  Edit which preset? [1-5]");
            while (!isKeyPressed()) ;
            key = getChar();
            if (key >= '1' && key <= '5') {
                idx = (uint8_t)(key - '1');
                _edit_preset(idx);
            } else {
                vdp_clearRows(20, 23);
            }
            _draw_menu();
            continue;
        }

        /* [Q] / [q] — quit program */
        if (key == 'Q' || key == 'q') {
            return false;
        }

        /* [D] / [d] — delete a preset */
        if (key == 'D' || key == 'd') {
            vdp_setCursor2(0, 20);
            vdp_clearRows(20, 23);
            vdp_setCursor2(0, 20);
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            vdp_print((uint8_t *)"  Delete which preset? [1-5]");
            while (!isKeyPressed()) ;
            key = getChar();
            if (key >= '1' && key <= '5') {
                idx = (uint8_t)(key - '1');
                _presets[idx].host[0] = 0;
                _presets[idx].port    = 23;
                _save_presets();
            }
            _draw_menu();
            continue;
        }
    }
}
