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
 * Builds:
 *   VDP_G2COL (default, stock NABU): 80-column virtual buffer, 32-column
 *   visible viewport.  Per-character colour via direct VRAM writes.
 *   Hardware note: the TMS9918A colour table is keyed by character code, not
 *   screen position -- every cell showing the same character gets the same
 *   colour.  splitThirds divides the screen into three independent 8-row
 *   bands (rows 0-7, 8-15, 16-23), each with its own colour table, so the
 *   same character can appear in different colours in different screen thirds.
 *
 *   VDP_80COL (F18A 80-column): direct VDP writes, global colour register.
 */

#include "ansi.h"

/* -----------------------------------------------------------------------
 * Parser states
 * --------------------------------------------------------------------- */
#define _ST_TEXT  0
#define _ST_ESC   1
#define _ST_CSI   2

#define _PB_SIZE    16
#define _MAX_PARAMS  4

/* -----------------------------------------------------------------------
 * ANSI SGR colour maps
 * --------------------------------------------------------------------- */
static const uint8_t _col[8] = {
    VDP_BLACK, VDP_MED_RED, VDP_MED_GREEN, VDP_DARK_YELLOW,
    VDP_DARK_BLUE, VDP_MAGENTA, VDP_CYAN, VDP_WHITE
};
static const uint8_t _bright[8] = {
    VDP_GRAY, VDP_LIGHT_RED, VDP_LIGHT_GREEN, VDP_LIGHT_YELLOW,
    VDP_LIGHT_BLUE, VDP_MAGENTA, VDP_CYAN, VDP_WHITE
};

/* -----------------------------------------------------------------------
 * Parser / SGR state -- declared early so G2 helpers can reference them
 * --------------------------------------------------------------------- */
static uint8_t _state;
static uint8_t _pbuf[_PB_SIZE];
static uint8_t _plen;

static uint8_t _fg;
static uint8_t _bg;
static uint8_t _bold;
static uint8_t _sv_x;
static uint8_t _sv_y;

#ifndef VDP_G2COL
static uint8_t _last_vdp_fg;
#endif

/* -----------------------------------------------------------------------
 * Virtual buffer -- G2 colour mode only
 * 80-column virtual screen; 32-column VDP viewport scrolled by _vp_x.
 * --------------------------------------------------------------------- */
#ifdef VDP_G2COL

static uint8_t _vbuf_char[24][80]; /* character at each logical cell     */
static uint8_t _vbuf_col[24][80];  /* packed colour: (fg<<4)|bg           */
static uint8_t _log_x;             /* logical cursor column  0-79         */
static uint8_t _log_y;             /* logical cursor row     0-23         */
static uint8_t _vp_x;              /* viewport left column   0-48         */
/* Colour to use for each character code in each screen band.
 * _vp_col[band][char_code] holds the packed colour byte (fg<<4)|bg.
 * The three bands cover rows 0-7, 8-15, and 16-23 (splitThirds layout);
 * each band has its own independent colour table so the same character
 * can be green in the top third and white in the middle third, for example.
 * Rebuilt each render by pass 1; used by pass 2 to write VRAM.
 * Static (not a local variable) to keep ~768 bytes off the Z80 stack. */
static uint8_t _vp_col[3][256];

/* Bit array: which pattern IDs are present in each band's viewport slice.
 * 256 bits = 32 bytes per band.  Bit (ch & 7) of byte (ch >> 3) is set when
 * pattern ID ch appears in the current viewport for that band.
 * Built by pass 1; drives pass 2 so only seen IDs get colour writes. */
static uint8_t _vp_seen[3][32];

/* Write colour (fg, bg) for pattern ch into band b of the VDP colour table.
 * Band 0 = rows 0-7, band 1 = rows 8-15, band 2 = rows 16-23.
 * Each pattern occupies 8 consecutive bytes in the colour table. */
static void _g2_colorize(uint8_t b, uint8_t ch, uint8_t fg, uint8_t bg)
{
    uint8_t c = (uint8_t)((fg << 4) | bg);
    uint8_t i;
    vdp_setWriteAddress(_vdpColorTableAddr
                        + (uint16_t)b  * 2048u
                        + (uint16_t)ch * 8u);
    for (i = 0u; i < 8u; i++)
        IO_VDPDATA = c;
}

/* Write colour (fg, bg) for pattern ch into all three VDP colour table bands. */
static void _g2_colorize_all(uint8_t ch, uint8_t fg, uint8_t bg)
{
    _g2_colorize(0u, ch, fg, bg);
    _g2_colorize(1u, ch, fg, bg);
    _g2_colorize(2u, ch, fg, bg);
}

/* Write one char to virtual buffer at (x,y) with current fg/bg;
 * also pushes to VDP if the cell falls within the visible viewport. */
static void _g2_put(uint8_t x, uint8_t y, uint8_t c)
{
    _vbuf_char[y][x] = c;
    _vbuf_col[y][x]  = (uint8_t)((_fg << 4) | _bg);
    if (x >= _vp_x && x < (uint8_t)(_vp_x + 32u)) {
        _g2_colorize((uint8_t)(y >> 3u), c, _fg, _bg);
        vdp_putPattern((uint8_t)(x - _vp_x), y, c);
    }
}

/* Forward declaration -- _g2_write calls _g2_lf on line wrap. */
static void _g2_lf(void);

/* Write c at logical cursor; advance cursor, wrapping at column 80. */
static void _g2_write(uint8_t c)
{
    _g2_put(_log_x, _log_y, c);
    if (_log_x < 79u) {
        _log_x++;
    } else {
        _log_x = 0;
        _g2_lf();
    }
}

/* Scroll virtual buffer up one row; blank row 23. */
static void _vbuf_scroll_up(void)
{
    uint8_t row, col;
    uint8_t packed;
    packed = (uint8_t)((VDP_WHITE << 4) | VDP_BLACK);
    for (row = 0; row < 23u; row++) {
        for (col = 0; col < 80u; col++) {
            _vbuf_char[row][col] = _vbuf_char[(uint8_t)(row + 1u)][col];
            _vbuf_col[row][col]  = _vbuf_col[(uint8_t)(row + 1u)][col];
        }
    }
    for (col = 0; col < 80u; col++) {
        _vbuf_char[23][col] = ' ';
        _vbuf_col[23][col]  = packed;
    }
}

/* Render the full 32x24 viewport from the virtual buffer to the VDP.
 *
 * The TMS9918A colour table is keyed by character code, not screen position
 * -- all cells showing the same character share one colour entry per band.
 * With splitThirds=true the chip provides three independent colour tables,
 * one per 8-row band, stored at offsets 0, 2048, and 4096 in VRAM.
 *
 * Three passes are used to avoid visible colour flash:
 *
 *   Pass 1 (CPU only)  -- scan the viewport and decide the final colour for
 *                         each character code in each band.  If the same
 *                         character appears at two different colours within a
 *                         band, the last one scanned wins (unavoidable --
 *                         the hardware allows only one colour per code per
 *                         band).  Also records which codes are actually on
 *                         screen so pass 2 can skip the rest.
 *   Pass 2 (VRAM)      -- write colour table entries to VRAM, but only for
 *                         character codes that actually appear in the viewport.
 *                         All colour writes happen before any character is
 *                         redrawn, so colours are stable when pass 3 runs.
 *                         Syncs to VBlank first to reduce screen tearing.
 *   Pass 3 (VRAM)      -- write the name table (which character goes where).
 *                         Colours are already set, so no flash is visible.
 */
void ansi_render_viewport(void)
{
    uint8_t  row, col, ch, cl, band, bm, bit, pid;
    uint16_t pi;

    /* Clear seen-bit arrays (96 bytes). */
    for (band = 0u; band < 3u; band++)
        for (col = 0u; col < 32u; col++)
            _vp_seen[band][col] = 0u;

    /* Pass 1: build _vp_col and _vp_seen for each band.
     * Band 0 = rows 0-7, band 1 = rows 8-15, band 2 = rows 16-23. */
    for (band = 0u; band < 3u; band++) {
        for (row = (uint8_t)(band * 8u);
             row < (uint8_t)(band * 8u + 8u);
             row++) {
            for (col = 0u; col < 32u; col++) {
                ch = _vbuf_char[row][(uint8_t)(_vp_x + col)];
                cl = _vbuf_col[row][(uint8_t)(_vp_x + col)];
                _vp_col[band][ch] = cl;
                _vp_seen[band][(uint8_t)(ch >> 3u)] |=
                    (uint8_t)(1u << (ch & 7u));
            }
        }
    }

    /* Wait for VBlank before writing VRAM to reduce visible tearing. */
    vdp_waitVDPReadyInt();

    /* Pass 2: write colour entries only for pattern IDs actually present
     * in the viewport.  Iterates the 32-byte seen-bit array per band;
     * typical BBS content uses ~50-80 distinct chars per band, so this
     * writes roughly 50-80 * 3 * (1 addr + 8 data) vs 256 * 3 * (1 addr
     * + 8 data) for a full table write.  Much faster in practice. */
    for (band = 0u; band < 3u; band++) {
        for (pi = 0u; pi < 32u; pi++) {
            bm = _vp_seen[band][(uint8_t)pi];
            if (!bm) continue;
            for (bit = 0u; bit < 8u; bit++) {
                if (bm & (uint8_t)(1u << bit)) {
                    pid = (uint8_t)((uint8_t)(pi << 3u) | bit);
                    cl  = _vp_col[band][pid];
                    vdp_setWriteAddress(_vdpColorTableAddr
                                        + (uint16_t)band * 2048u
                                        + (uint16_t)pid  * 8u);
                    IO_VDPDATA = cl; IO_VDPDATA = cl;
                    IO_VDPDATA = cl; IO_VDPDATA = cl;
                    IO_VDPDATA = cl; IO_VDPDATA = cl;
                    IO_VDPDATA = cl; IO_VDPDATA = cl;
                }
            }
        }
    }

    /* Pass 3: update name table -- colours are stable, no flash. */
    for (row = 0u; row < 24u; row++) {
        for (col = 0u; col < 32u; col++) {
            vdp_putPattern(col, row,
                           _vbuf_char[row][(uint8_t)(_vp_x + col)]);
        }
    }
}

/* Move logical cursor down one row; scroll virtual buffer if at bottom. */
static void _g2_lf(void)
{
    if (_log_y < 23u) {
        _log_y++;
    } else {
        _vbuf_scroll_up();
        ansi_render_viewport();
    }
}

/* Clear rows top..bot in virtual buffer and on the visible VDP rows. */
static void _g2_clearrows(uint8_t top, uint8_t bot)
{
    uint8_t row, col;
    uint8_t packed;
    packed = (uint8_t)((VDP_WHITE << 4) | VDP_BLACK);
    for (row = top; row <= bot; row++) {
        for (col = 0; col < 80u; col++) {
            _vbuf_char[row][col] = ' ';
            _vbuf_col[row][col]  = packed;
        }
    }
    /* Ensure space has white-on-black colour in all three bands. */
    _g2_colorize_all(' ', VDP_WHITE, VDP_BLACK);
    for (row = top; row <= bot; row++) {
        for (col = 0; col < 32u; col++) {
            vdp_putPattern(col, row, ' ');
        }
    }
}

/* Scroll visible viewport left by one column; re-render. */
void ansi_viewport_left(void)
{
    if (_vp_x > 0) {
        _vp_x--;
        ansi_render_viewport();
    }
}

/* Scroll visible viewport right by one column; re-render. */
void ansi_viewport_right(void)
{
    if (_vp_x < 48u) {
        _vp_x++;
        ansi_render_viewport();
    }
}

/* Jump viewport left by 8 columns (Page Back key). */
void ansi_viewport_page_left(void)
{
    _vp_x = (_vp_x >= 8u) ? (uint8_t)(_vp_x - 8u) : 0;
    ansi_render_viewport();
}

/* Jump viewport right by 8 columns (Page Fwd key). */
void ansi_viewport_page_right(void)
{
    _vp_x = (_vp_x <= 40u) ? (uint8_t)(_vp_x + 8u) : 48u;
    ansi_render_viewport();
}

#endif /* VDP_G2COL */

/* -----------------------------------------------------------------------
 * Mode-specific aliases
 *   _CUR_X/_CUR_Y  -- current cursor position (logical in G2, VDP in text)
 *   _SET_CUR(x,y)  -- position cursor
 * --------------------------------------------------------------------- */
#ifdef VDP_G2COL
#  define _CUR_X  _log_x
#  define _CUR_Y  _log_y
#  define _SET_CUR(x, y)  do { _log_x = (x); _log_y = (y); } while (0)
#else
#  define _CUR_X  vdp_cursor.x
#  define _CUR_Y  vdp_cursor.y
#  define _SET_CUR(x, y)  vdp_setCursor2((x), (y))
#endif

#ifndef VDP_G2COL
/* Colour cycle for 80-col mode.  SGR colour codes are not applied to the
 * VDP colour register mid-stream -- doing so flashes the entire screen.
 * The user cycles through this table with Ctrl+T. */
static const uint8_t _80col_colours[7] = {
    VDP_WHITE, VDP_CYAN, VDP_LIGHT_GREEN, VDP_LIGHT_YELLOW,
    VDP_LIGHT_BLUE, VDP_MAGENTA, VDP_LIGHT_RED
};
static uint8_t _80col_ci = 0; /* index into _80col_colours */

void ansi_cycle_colour(void)
{
    _80col_ci = (uint8_t)(_80col_ci + 1u);
    if (_80col_ci >= 7u) _80col_ci = 0u;
    vdp_setTextColor(_80col_colours[_80col_ci], VDP_BLACK);
}
#endif /* !VDP_G2COL */

/* -----------------------------------------------------------------------
 * Help overlay
 *
 * Writes a box directly to the VDP name table without touching the virtual
 * buffer (_vbuf_char) or _vdp_textBuffer, so the original screen content
 * is preserved and can be restored after dismissal.
 *
 * G2 mode:   restore by calling ansi_render_viewport() -- free.
 * 80-col:    restore by replaying _vdp_textBuffer back to VRAM.
 * --------------------------------------------------------------------- */

/* Write one character directly to the VDP name table at physical (x, y).
 * Uses _vdpCursorMaxXFull (32 for G2, 80 for TEXT80) as the row stride. */
static void _help_put(uint8_t x, uint8_t y, uint8_t ch)
{
    vdp_setWriteAddress(_vdpPatternNameTableAddr
                        + (uint16_t)y * (uint16_t)_vdpCursorMaxXFull
                        + (uint16_t)x);
    IO_VDPDATA = ch;
}

/* Write a null-terminated string directly to the VDP name table at (x, y). */
static void _help_puts(uint8_t x, uint8_t y, uint8_t *s)
{
    while (*s) {
        _help_put(x, y, *s);
        x++;
        s++;
    }
}

/* Show the key-reference overlay and block until any key is pressed.
 * G2:   box at col 1, width 30 (inner 28), rows 8-14.
 * 80-col: box at col 19, width 42 (inner 40), rows 9-16. */
void ansi_show_help(void)
{
    uint8_t i;

#ifdef VDP_G2COL
    /* Top border */
    _help_put(1u, 8u, 0xC9u);
    for (i = 2u; i <= 29u; i++) _help_put(i, 8u, 0xCDu);
    _help_put(30u, 8u, 0xBBu);
    /* Title row */
    _help_put(1u, 9u, 0xBAu);
    _help_puts(2u, 9u, (uint8_t *)"   NABU Terminal  " NABUTERM_VERSION "  ");
    _help_put(30u, 9u, 0xBAu);
    /* Separator */
    _help_put(1u, 10u, 0xCCu);
    for (i = 2u; i <= 29u; i++) _help_put(i, 10u, 0xCDu);
    _help_put(30u, 10u, 0xB9u);
    /* Key rows */
    _help_put(1u, 11u, 0xBAu);
    _help_puts(2u, 11u, (uint8_t *)" L/R arrows  scroll 1 col   ");
    _help_put(30u, 11u, 0xBAu);
    _help_put(1u, 12u, 0xBAu);
    _help_puts(2u, 12u, (uint8_t *)" Pg L/R      scroll 8 cols  ");
    _help_put(30u, 12u, 0xBAu);
    _help_put(1u, 13u, 0xBAu);
    _help_puts(2u, 13u, (uint8_t *)" SYM key     this help      ");
    _help_put(30u, 13u, 0xBAu);
    /* Bottom border */
    _help_put(1u, 14u, 0xC8u);
    for (i = 2u; i <= 29u; i++) _help_put(i, 14u, 0xCDu);
    _help_put(30u, 14u, 0xBCu);
    /* Wait for any key, then restore from virtual buffer */
    while (!isKeyPressed()) ;
    getChar();
    ansi_render_viewport();
#else
    {
        uint8_t row, col;
        /* Top border */
        _help_put(19u, 9u, 0xC9u);
        for (i = 20u; i <= 59u; i++) _help_put(i, 9u, 0xCDu);
        _help_put(60u, 9u, 0xBBu);
        /* Title */
        _help_put(19u, 10u, 0xBAu);
        _help_puts(20u, 10u,
            (uint8_t *)"   NABU BBS Terminal  " NABUTERM_VERSION "  F18A    ");
        _help_put(60u, 10u, 0xBAu);
        /* Separator */
        _help_put(19u, 11u, 0xCCu);
        for (i = 20u; i <= 59u; i++) _help_put(i, 11u, 0xCDu);
        _help_put(60u, 11u, 0xB9u);
        /* Key rows */
        _help_put(19u, 12u, 0xBAu);
        _help_puts(20u, 12u,
            (uint8_t *)" Ctrl+T     Cycle text colour           ");
        _help_put(60u, 12u, 0xBAu);
        _help_put(19u, 13u, 0xBAu);
        _help_puts(20u, 13u,
            (uint8_t *)" Ctrl+]     Disconnect                  ");
        _help_put(60u, 13u, 0xBAu);
        _help_put(19u, 14u, 0xBAu);
        _help_puts(20u, 14u,
            (uint8_t *)" Ctrl+E     Toggle local echo           ");
        _help_put(60u, 14u, 0xBAu);
        _help_put(19u, 15u, 0xBAu);
        _help_puts(20u, 15u,
            (uint8_t *)" SYM key    This help                   ");
        _help_put(60u, 15u, 0xBAu);
        /* Bottom border */
        _help_put(19u, 16u, 0xC8u);
        for (i = 20u; i <= 59u; i++) _help_put(i, 16u, 0xCDu);
        _help_put(60u, 16u, 0xBCu);
        /* Wait for any key, then restore affected rows from _vdp_textBuffer.
         * _vdp_textBuffer was not updated (we wrote directly to VRAM), so
         * it still holds the original screen content. */
        while (!isKeyPressed()) ;
        getChar();
        for (row = 9u; row <= 16u; row++) {
            vdp_setWriteAddress(_vdpPatternNameTableAddr
                                + (uint16_t)row
                                  * (uint16_t)_vdpCursorMaxXFull);
            for (col = 0u; col < 80u; col++)
                IO_VDPDATA =
                    _vdp_textBuffer[(uint16_t)row * 80u + col];
        }
    }
#endif
}

/* -----------------------------------------------------------------------
 * _parse_params
 * --------------------------------------------------------------------- */
static uint8_t _parse_params(uint16_t *out, uint8_t maxp)
{
    uint8_t  n = 0, i;
    uint16_t v = 0;
    uint8_t  any = 0;

    for (i = 0; i < _plen; i++) {
        uint8_t c = _pbuf[i];
        if (c >= '0' && c <= '9') {
            v = (uint16_t)(v * 10u + (uint16_t)(c - '0'));
            any = 1;
        } else if (c == ';') {
            if (n < maxp) out[n++] = any ? v : 0;
            v = 0; any = 0;
        }
    }
    if (any && n < maxp)
        out[n++] = v;
    return n;
}

/* -----------------------------------------------------------------------
 * _sgr
 * --------------------------------------------------------------------- */
static void _sgr(uint16_t *p, uint8_t np)
{
    uint8_t  i;
    uint16_t v;
    uint8_t  tmp;

    if (np == 0) {
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
        } else if (v == 4 || v == 5 || v == 6) {
            /* underline / blink -- ignore */
        } else if (v == 7) {
            /* reverse: swap fg/bg, approximate with gray fg */
            tmp = _fg; _fg = VDP_GRAY; _bg = tmp;
        } else if (v == 27) {
            _fg = VDP_WHITE; _bg = VDP_BLACK;
        } else if (v >= 30 && v <= 37) {
            _fg = _bold ? _bright[v - 30] : _col[v - 30];
        } else if (v == 39) {
            _fg = VDP_WHITE;
        } else if (v >= 40 && v <= 47) {
            _bg = _col[v - 40];
        } else if (v == 49) {
            _bg = VDP_BLACK;
        } else if (v >= 90 && v <= 97) {
            _fg = _bright[v - 90];
        } else if (v >= 100 && v <= 107) {
            _bg = _bright[v - 100];
        }
    }
    /* _fg and _bg are updated above.  In G2 mode they are applied per cell
     * at write time.  In 80-col mode they are intentionally not written to
     * the VDP colour register (Ctrl+T controls the global colour instead). */
}

/* -----------------------------------------------------------------------
 * _dispatch_csi
 * --------------------------------------------------------------------- */
static void _dispatch_csi(uint8_t cmd)
{
    uint16_t p[_MAX_PARAMS];
    uint8_t  np;
    uint8_t  n8;
    uint8_t  row, col;
    uint8_t  cx, cy;
    uint8_t  j;

    p[0] = p[1] = p[2] = p[3] = 0;
    np = _parse_params(p, _MAX_PARAMS);

    switch (cmd) {

    /* -- Cursor Position (CUP / HVP) ---------------------------------- */
    case 'H':
    case 'f':
        row = (p[0] > 0) ? (uint8_t)(p[0] - 1u) : 0;
        col = (p[1] > 0) ? (uint8_t)(p[1] - 1u) : 0;
        if (row > 23) row = 23;
        if (col > (uint8_t)(SCREEN_COLS - 1)) col = (uint8_t)(SCREEN_COLS - 1);
        _SET_CUR(col, row);
        break;

    /* -- Cursor Up (CUU) ---------------------------------------------- */
    case 'A':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        row = _CUR_Y;
        _SET_CUR(_CUR_X, (row >= n8) ? (uint8_t)(row - n8) : 0);
        break;

    /* -- Cursor Down (CUD) -------------------------------------------- */
    case 'B':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        row = (uint8_t)(_CUR_Y + n8);
        if (row > 23) row = 23;
        _SET_CUR(_CUR_X, row);
        break;

    /* -- Cursor Forward/Right (CUF) ----------------------------------- */
    case 'C':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        col = (uint8_t)(_CUR_X + n8);
        if (col > (uint8_t)(SCREEN_COLS - 1)) col = (uint8_t)(SCREEN_COLS - 1);
        _SET_CUR(col, _CUR_Y);
        break;

    /* -- Cursor Backward/Left (CUB) ----------------------------------- */
    case 'D':
        n8  = (uint8_t)(p[0] > 0 ? p[0] : 1);
        col = _CUR_X;
        _SET_CUR((col >= n8) ? (uint8_t)(col - n8) : 0, _CUR_Y);
        break;

    /* -- Erase Display (ED) ------------------------------------------- */
    case 'J':
        cx = _CUR_X; cy = _CUR_Y;
        switch ((uint8_t)p[0]) {
        case 0:
            /* Erase cursor to end of screen */
#ifdef VDP_G2COL
            {
                uint8_t r2, c2;
                uint8_t pk = (uint8_t)((VDP_WHITE << 4) | VDP_BLACK);
                for (c2 = cx; c2 < 80u; c2++) {
                    _vbuf_char[cy][c2] = ' ';
                    _vbuf_col[cy][c2]  = pk;
                }
                for (r2 = (uint8_t)(cy + 1u); r2 < 24u; r2++) {
                    for (c2 = 0; c2 < 80u; c2++) {
                        _vbuf_char[r2][c2] = ' ';
                        _vbuf_col[r2][c2]  = pk;
                    }
                }
                ansi_render_viewport();
            }
#else
            for (j = cx; j < (uint8_t)SCREEN_COLS; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
            if (cy < 23) vdp_clearRows((uint8_t)(cy + 1), 23);
#endif
            break;
        case 1:
            /* Erase start of screen to cursor -- approximate: ignore */
            break;
        case 2:
            /* Erase entire screen */
#ifdef VDP_G2COL
            {
                uint8_t r2, c2;
                uint8_t pk = (uint8_t)((VDP_WHITE << 4) | VDP_BLACK);
                for (r2 = 0; r2 < 24u; r2++)
                    for (c2 = 0; c2 < 80u; c2++) {
                        _vbuf_char[r2][c2] = ' ';
                        _vbuf_col[r2][c2]  = pk;
                    }
                vdp_clearScreen();
                vdp_setCursor2(0, 0);
            }
#else
            vdp_clearScreen();
#endif
            break;
        }
        break;

    /* -- Erase Line (EL) ---------------------------------------------- */
    case 'K':
        cx = _CUR_X; cy = _CUR_Y;
        switch ((uint8_t)p[0]) {
        case 0:
            /* Erase cursor to end of line */
#ifdef VDP_G2COL
            for (j = cx; j < 80u; j++) _g2_put(j, cy, ' ');
#else
            for (j = cx; j < (uint8_t)SCREEN_COLS; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
#endif
            break;
        case 1:
            /* Erase start of line to cursor */
#ifdef VDP_G2COL
            for (j = 0; j <= cx; j++) _g2_put(j, cy, ' ');
#else
            vdp_setCursor2(0, cy);
            for (j = 0; j <= cx; j++) vdp_write(' ');
            vdp_setCursor2(cx, cy);
#endif
            break;
        case 2:
            /* Erase entire line */
#ifdef VDP_G2COL
            _g2_clearrows(cy, cy);
#else
            vdp_clearRows(cy, cy);
            vdp_setCursor2(cx, cy);
#endif
            break;
        }
        break;

    /* -- Save / Restore Cursor ---------------------------------------- */
    case 's':
        _sv_x = _CUR_X; _sv_y = _CUR_Y;
        break;
    case 'u':
        _SET_CUR(_sv_x, _sv_y);
        break;

    /* -- SGR ---------------------------------------------------------- */
    case 'm':
        _sgr(p, np);
        break;

    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void ansi_reset(void)
{
    _state = _ST_TEXT;
    _plen  = 0;
    _fg    = VDP_WHITE;
    _bg    = VDP_BLACK;
    _bold  = 0;
    _sv_x  = 0;
    _sv_y  = 0;

#ifdef VDP_G2COL
    {
        uint8_t  row, col, band;
        uint16_t pi;
        uint8_t  packed;
        packed = (uint8_t)((VDP_WHITE << 4) | VDP_BLACK);
        _log_x = 0;
        _log_y = 0;
        _vp_x  = 0;
        for (row = 0u; row < 24u; row++) {
            for (col = 0u; col < 80u; col++) {
                _vbuf_char[row][col] = ' ';
                _vbuf_col[row][col]  = packed;
            }
        }
        /* Init _vp_col for all three bands so the first render has valid data
         * for pattern IDs not yet seen in the viewport. */
        for (pi = 0u; pi < 256u; pi++) {
            _vp_col[0][pi] = packed;
            _vp_col[1][pi] = packed;
            _vp_col[2][pi] = packed;
        }
        /* Write white-on-black to all three VDP colour table bands in
         * sequential 2048-byte bursts (256 patterns * 8 bytes each). */
        for (band = 0u; band < 3u; band++) {
            vdp_setWriteAddress(_vdpColorTableAddr + (uint16_t)band * 2048u);
            for (pi = 0u; pi < 2048u; pi++)
                IO_VDPDATA = packed;
        }
    }
#else
    /* Apply the user-selected cycle colour; do not reset the cycle index
     * (_80col_ci) so the user's chosen colour persists across sessions. */
    vdp_setTextColor(_80col_colours[_80col_ci], VDP_BLACK);
#endif
}

void ansi_feed(uint8_t c)
{
    uint8_t nx;

    switch (_state) {

    /* ----------------------------------------------------------------- */
    case _ST_TEXT:
        if (c == 0x1B) { _state = _ST_ESC; return; }

        /* --- Control characters -------------------------------------- */
        if (c == 0x0D) {
            /* CR: return to column 0 */
#ifdef VDP_G2COL
            _log_x = 0;
#else
            vdp_setCursor2(0, vdp_cursor.y);
#endif
            return;
        }
        if (c == 0x0A) {
            /* LF: move down one row (do NOT reset column -- telnet NVT) */
#ifdef VDP_G2COL
            _g2_lf();
#else
            vdp_newLine();
            vdp_setCursor2(0, vdp_cursor.y);
#endif
            return;
        }
        if (c == 0x08 || c == 0x7F) {
            /* Destructive backspace */
#ifdef VDP_G2COL
            if (_log_x > 0) {
                _log_x--;
                _g2_put(_log_x, _log_y, ' ');
            }
#else
            if (vdp_cursor.x > 0) {
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
                vdp_write(' ');
                vdp_setCursor2((uint8_t)(vdp_cursor.x - 1), vdp_cursor.y);
            }
#endif
            return;
        }
        if (c == 0x09) {
            /* HT: advance to next 8-column tab stop */
#ifdef VDP_G2COL
            nx = (uint8_t)((_log_x + 8u) & ~7u);
            if (nx < 80u) _log_x = nx;
#else
            nx = (uint8_t)((vdp_cursor.x + 8) & ~7u);
            if (nx < (uint8_t)SCREEN_COLS) vdp_setCursor2(nx, vdp_cursor.y);
#endif
            return;
        }
        if (c == 0x07) return; /* BEL -- ignore */

        /* --- Printable: ASCII + CP437 extended ----------------------- */
        if ((c >= 0x20 && c < 0x7F) || c >= 0x80) {
#ifdef VDP_G2COL
            _g2_write(c);
#else
            /* SGR colour changes are not applied here -- writing the
             * global colour register mid-stream flashes the entire
             * 80-col screen.  Colour is set by Ctrl+T (cycle) only. */
            vdp_write(c);
#endif
        }
        return;

    /* ----------------------------------------------------------------- */
    case _ST_ESC:
        if (c == '[') {
            _state = _ST_CSI;
            _plen  = 0;
            return;
        }
        if (c == '7') {
            _sv_x = _CUR_X; _sv_y = _CUR_Y;
            _state = _ST_TEXT;
            return;
        }
        if (c == '8') {
            _SET_CUR(_sv_x, _sv_y);
            _state = _ST_TEXT;
            return;
        }
        if (c == 'c') {
            /* Full terminal reset */
            ansi_reset();
#ifdef VDP_G2COL
            vdp_clearScreen();
            vdp_setCursor2(0, 0);
#else
            vdp_clearScreen();
#endif
            _state = _ST_TEXT;
            return;
        }
        /* Unknown ESC sequence: discard ESC, re-process byte as text */
        _state = _ST_TEXT;
        ansi_feed(c);
        return;

    /* ----------------------------------------------------------------- */
    case _ST_CSI:
        if (c >= '0' && c <= ';') {
            if (_plen < _PB_SIZE - 1)
                _pbuf[_plen++] = c;
            return;
        }
        if (c == '?' && _plen == 0) {
            _pbuf[_plen++] = c;
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {
            if (!(_plen > 0 && _pbuf[0] == '?'))
                _dispatch_csi(c);
        }
        _state = _ST_TEXT;
        _plen  = 0;
        return;
    }
}
