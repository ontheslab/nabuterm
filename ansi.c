/*
 * ansi.c -- ANSI/VT100 escape sequence parser
 *
 * Included into nterm.c via  #include "ansi.c"
 * Do NOT include NABU-LIB.h here; it is already included by nterm.c.
 *
 * Handles the subset of ANSI/VT100 sequences common in BBS ANSI art:
 *   - Cursor movement  : CUU/CUD/CUF/CUB (A-D), CUP/HVP (H/f)
 *   - Cursor save/restore: ESC 7 / ESC 8, CSI s / CSI u
 *   - Erase            : ED (J), EL (K)
 *   - SGR attributes   : colours, bold, reverse, reset (m)
 *   - Private sequences (ESC[?...): recognised and silently discarded
 *
 * Parameter scanning uses the dctelnet trick: collect while byte is
 * in range '0'..'<semi>' (0x30..0x3B) — catches digits and semicolons.
 *
 * VDP gotcha: vdp_newLine() does NOT reset cursor column.
 * Always follow it with vdp_setCursor2(0, vdp_cursor.y).
 */

#include "ansi.h"

/* -----------------------------------------------------------------------
 * Parser states
 * --------------------------------------------------------------------- */
#define _ST_TEXT  0   /* normal data                         */
#define _ST_ESC   1   /* seen ESC (0x1B)                     */
#define _ST_CSI   2   /* seen ESC [ — collecting parameters  */

/* -----------------------------------------------------------------------
 * Parameter buffer
 * Stores the raw parameter string, e.g. "31;1;42" for ESC[31;1;42m
 * Max params in a BBS sequence: typically 2-3; 16 bytes is ample.
 * --------------------------------------------------------------------- */
#define _PB_SIZE   16
#define _MAX_PARAMS 4

/* -----------------------------------------------------------------------
 * ANSI standard (30-37 / 40-47) → VDP colour
 * --------------------------------------------------------------------- */
static const uint8_t _col[8] = {
    VDP_BLACK,        /* 0 */
    VDP_MED_RED,      /* 1 */
    VDP_MED_GREEN,    /* 2 */
    VDP_DARK_YELLOW,  /* 3 */
    VDP_DARK_BLUE,    /* 4 */
    VDP_MAGENTA,      /* 5 */
    VDP_CYAN,         /* 6 */
    VDP_WHITE         /* 7 */
};

/* ANSI bright (90-97 / 100-107) → VDP colour */
static const uint8_t _bright[8] = {
    VDP_GRAY,         /* 0 bright black  */
    VDP_LIGHT_RED,    /* 1               */
    VDP_LIGHT_GREEN,  /* 2               */
    VDP_LIGHT_YELLOW, /* 3               */
    VDP_LIGHT_BLUE,   /* 4               */
    VDP_MAGENTA,      /* 5 (no brighter) */
    VDP_CYAN,         /* 6 (no brighter) */
    VDP_WHITE         /* 7               */
};

/* -----------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------- */
static uint8_t _state;
static uint8_t _pbuf[_PB_SIZE];  /* parameter string buffer */
static uint8_t _plen;            /* bytes used in _pbuf     */

/* Current SGR colour state */
static uint8_t _fg;
static uint8_t _bg;
static uint8_t _bold;
static uint8_t _last_vdp_fg;  /* last fg actually written to VDP reg 7 */

/* Saved cursor position (ESC 7 / ESC 8 / CSI s / CSI u) */
static uint8_t _sv_x;
static uint8_t _sv_y;

/* -----------------------------------------------------------------------
 * _parse_params
 * Parse _pbuf into out[0..maxp-1]. Returns count of params found.
 * Empty/omitted params are left as 0 (caller treats 0 as "default").
 * --------------------------------------------------------------------- */
static uint8_t _parse_params(uint16_t *out, uint8_t maxp)
{
    uint8_t  n = 0, i;
    uint16_t v = 0;
    uint8_t  any = 0;

    for (i = 0; i < _plen; i++) {
        uint8_t c = _pbuf[i];
        if (c >= '0' && c <= '9') {
            v = (uint16_t)(v * 10 + (c - '0'));
            any = 1;
        } else if (c == ';') {
            if (n < maxp) out[n++] = any ? v : 0;
            v = 0; any = 0;
        }
        /* other chars (':', '?') — ignored by parser */
    }
    if (any && n < maxp)
        out[n++] = v;
    return n;
}

/* -----------------------------------------------------------------------
 * _apply_colour — push fg to VDP only when it has changed.
 * Background is always VDP_BLACK: vdp_setTextColor writes register 7
 * which instantly repaints ALL characters on screen, so we minimise
 * calls and never let a BBS background colour obscure the text.
 * --------------------------------------------------------------------- */
static void _apply_colour(void)
{
    if (_fg != _last_vdp_fg) {
        vdp_setTextColor(_fg, VDP_BLACK);
        _last_vdp_fg = _fg;
    }
}

/* -----------------------------------------------------------------------
 * _sgr — process SGR (Select Graphic Rendition) parameters
 * --------------------------------------------------------------------- */
static void _sgr(uint16_t *p, uint8_t np)
{
    uint8_t  i;
    uint16_t v;
    uint8_t  tmp;

    if (np == 0) {
        /* ESC[m — full reset: record new colour but defer VDP write */
        _fg = VDP_WHITE; _bg = VDP_BLACK; _bold = 0;
        return;
    }

    for (i = 0; i < np; i++) {
        v = p[i];
        if (v == 0) {
            _fg = VDP_WHITE; _bg = VDP_BLACK; _bold = 0;
        } else if (v == 1) {
            _bold = 1;
        } else if (v == 2 || v == 22) {
            _bold = 0;
        } else if (v == 5 || v == 6) {
            /* blink — ignore */
        } else if (v == 4) {
            /* underline — ignore (no VDP support) */
        } else if (v == 7) {
            /* reverse video — bg is always black, approximate with gray */
            tmp = _fg;
            _fg = VDP_GRAY;
            _bg = tmp;
        } else if (v == 27) {
            /* reverse off — restore defaults */
            _fg = VDP_WHITE; _bg = VDP_BLACK;
        } else if (v >= 30 && v <= 37) {
            _fg = _bold ? _bright[v - 30] : _col[v - 30];
        } else if (v == 39) {
            _fg = VDP_WHITE;    /* default foreground */
        } else if (v >= 40 && v <= 47) {
            _bg = _col[v - 40];
        } else if (v == 49) {
            _bg = VDP_BLACK;    /* default background */
        } else if (v >= 90 && v <= 97) {
            _fg = _bright[v - 90];
        } else if (v >= 100 && v <= 107) {
            _bg = _bright[v - 100];
        }
    }
    /* Colour change deferred: VDP register 7 is global and repaints the
     * entire screen instantly.  We apply it only when a visible character
     * is about to be written (see ansi_feed) so that a burst of SGR codes
     * triggers at most one repaint per drawn character, not per sequence. */
}

/* -----------------------------------------------------------------------
 * _dispatch_csi — act on a completed CSI sequence
 * cmd : the final byte (letter) of the sequence
 * --------------------------------------------------------------------- */
static void _dispatch_csi(uint8_t cmd)
{
    uint16_t p[_MAX_PARAMS];
    uint8_t  np;
    uint8_t  n8;        /* scratch: movement count as uint8_t  */
    uint8_t  row, col;  /* scratch: cursor row/col              */
    uint8_t  cx, cy;    /* scratch: saved cursor position       */
    uint8_t  j;         /* scratch: loop index                  */

    p[0] = p[1] = p[2] = p[3] = 0;
    np = _parse_params(p, _MAX_PARAMS);

    switch (cmd) {

    /* -- Cursor Position (CUP / HVP) ---------------------------------- */
    case 'H':
    case 'f':
        row = (p[0] > 0) ? (uint8_t)(p[0] - 1) : 0;
        col = (p[1] > 0) ? (uint8_t)(p[1] - 1) : 0;
        if (row > 23) row = 23;
        if (col > (uint8_t)(SCREEN_COLS - 1)) col = (uint8_t)(SCREEN_COLS - 1);
        vdp_setCursor2(col, row);
        break;

    /* -- Cursor Up (CUU) ---------------------------------------------- */
    case 'A':
        n8 = (uint8_t)(p[0] > 0 ? p[0] : 1);
        row = vdp_cursor.y;
        vdp_setCursor2(vdp_cursor.x, (row >= n8) ? (uint8_t)(row - n8) : 0);
        break;

    /* -- Cursor Down (CUD) -------------------------------------------- */
    case 'B':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        row = (uint8_t)(vdp_cursor.y + n8);
        if (row > 23) row = 23;
        vdp_setCursor2(vdp_cursor.x, row);
        break;

    /* -- Cursor Forward/Right (CUF) ----------------------------------- */
    case 'C':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        col = (uint8_t)(vdp_cursor.x + n8);
        if (col > (uint8_t)(SCREEN_COLS - 1)) col = (uint8_t)(SCREEN_COLS - 1);
        vdp_setCursor2(col, vdp_cursor.y);
        break;

    /* -- Cursor Backward/Left (CUB) ----------------------------------- */
    case 'D':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        col = vdp_cursor.x;
        vdp_setCursor2((col >= n8) ? (uint8_t)(col - n8) : 0, vdp_cursor.y);
        break;

    /* -- Erase Display (ED) ------------------------------------------- */
    case 'J':
        switch ((uint8_t)p[0]) {
        case 0:
            /* Erase cursor to end of screen */
            cx = vdp_cursor.x; cy = vdp_cursor.y;
            for (j = cx; j < (uint8_t)SCREEN_COLS; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
            if (cy < 23) vdp_clearRows((uint8_t)(cy + 1), 23);
            break;
        case 1:
            /* Erase start of screen to cursor — approximate: ignore */
            break;
        case 2:
            vdp_clearScreen();
            break;
        }
        break;

    /* -- Erase Line (EL) ---------------------------------------------- */
    case 'K':
        cx = vdp_cursor.x; cy = vdp_cursor.y;
        switch ((uint8_t)p[0]) {
        case 0:
            /* Erase cursor to end of line */
            for (j = cx; j < (uint8_t)SCREEN_COLS; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
            break;
        case 1:
            /* Erase start of line to cursor */
            vdp_setCursor2(0, cy);
            for (j = 0; j <= cx; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
            break;
        case 2:
            /* Erase entire line */
            vdp_clearRows(cy, cy);
            vdp_setCursor2(cx, cy);
            break;
        }
        break;

    /* -- Save / Restore Cursor (SCOSC / SCORC) ------------------------ */
    case 's':
        _sv_x = vdp_cursor.x; _sv_y = vdp_cursor.y;
        break;
    case 'u':
        vdp_setCursor2(_sv_x, _sv_y);
        break;

    /* -- SGR — colours and attributes --------------------------------- */
    case 'm':
        _sgr(p, np);
        break;

    /* -- All others: silently discard --------------------------------- */
    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void ansi_reset(void)
{
    _state      = _ST_TEXT;
    _plen       = 0;
    _fg         = VDP_WHITE;
    _bg         = VDP_BLACK;
    _bold       = 0;
    _sv_x       = 0;
    _sv_y       = 0;
    _last_vdp_fg = 0xFF;         /* force a colour write on first use */
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
    _last_vdp_fg = VDP_WHITE;
}

void ansi_feed(uint8_t c)
{
    uint8_t nx; /* scratch for tab calculation */

    switch (_state) {

    /* ----------------------------------------------------------------- */
    case _ST_TEXT:
        if (c == 0x1B) { _state = _ST_ESC; return; }

        /* Control characters */
        if (c == 0x0D) {
            vdp_setCursor2(0, vdp_cursor.y);
            return;
        }
        if (c == 0x0A) {
            vdp_newLine();
            vdp_setCursor2(0, vdp_cursor.y); /* vdp_newLine doesn't reset col */
            return;
        }
        if (c == 0x08) {
            /* Destructive backspace: erase the character to the left */
            if (vdp_cursor.x > 0) {
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
                vdp_write(' ');
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
            }
            return;
        }
        if (c == 0x7F) {
            if (vdp_cursor.x > 0) {
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
                vdp_write(' ');
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
            }
            return;
        }
        if (c == 0x09) {
            /* HT: next 8-column tab stop */
            nx = (uint8_t)((vdp_cursor.x + 8) & ~7u);
            if (nx < (uint8_t)SCREEN_COLS) vdp_setCursor2(nx, vdp_cursor.y);
            return;
        }
        if (c == 0x07) return; /* BEL — ignore */

        /* Printable chars (ASCII + CP437 high bytes): apply pending colour
         * change here so a burst of SGR codes only triggers one VDP
         * register write per drawn character, not one per sequence. */
        if ((c >= 0x20 && c < 0x7F) || c >= 0x80) {
            _apply_colour();
            vdp_write(c);
        }
        /* Other control chars: silently drop */
        return;

    /* ----------------------------------------------------------------- */
    case _ST_ESC:
        if (c == '[') {
            /* Start of CSI sequence */
            _state = _ST_CSI;
            _plen  = 0;
            return;
        }
        if (c == '7') {
            /* DEC save cursor */
            _sv_x = vdp_cursor.x; _sv_y = vdp_cursor.y;
            _state = _ST_TEXT;
            return;
        }
        if (c == '8') {
            /* DEC restore cursor */
            vdp_setCursor2(_sv_x, _sv_y);
            _state = _ST_TEXT;
            return;
        }
        if (c == 'c') {
            /* Full terminal reset */
            ansi_reset();
            vdp_clearScreen();
            _state = _ST_TEXT;
            return;
        }
        /* Any other ESC sequence: discard the ESC, re-process c as text */
        _state = _ST_TEXT;
        ansi_feed(c);
        return;

    /* ----------------------------------------------------------------- */
    case _ST_CSI:
        /* Collect parameter bytes: digits 0-9 and semicolon separator.
         * Range '0'..'<semi>' (0x30-0x3B) via the dctelnet scan trick. */
        if (c >= '0' && c <= ';') {
            if (_plen < _PB_SIZE - 1)
                _pbuf[_plen++] = c;
            return;
        }

        /* '?' marks a private sequence (e.g. ESC[?25h cursor visibility).
         * Only valid as the very first byte after '['. */
        if (c == '?' && _plen == 0) {
            _pbuf[_plen++] = c;
            return;
        }

        /* Final byte: letters 0x40-0x7E are valid CSI terminators. */
        if (c >= 0x40 && c <= 0x7E) {
            /* Ignore private sequences (ESC[?...) entirely */
            if (!(_plen > 0 && _pbuf[0] == '?')) {
                _dispatch_csi(c);
            }
        }
        /* Any other byte terminates the sequence without dispatching */
        _state = _ST_TEXT;
        _plen  = 0;
        return;
    }
}
