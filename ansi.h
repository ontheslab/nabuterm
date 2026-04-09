/*
 * ansi.h -- ANSI/VT100 escape sequence parser interface
 */

#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>
#include <stdbool.h>

/* Reset all parser state and colour attributes.
 * In G2 mode: clears virtual buffer, writes fixed name table, blanks all
 * per-cell pattern/colour slots.  Call once before each new connection. */
void ansi_reset(void);

/* Feed one display byte (post-telnet-strip) into the parser.
 * Handles printable chars, control chars, and ESC sequences. */
void ansi_feed(uint8_t c);

#ifdef VDP_G2COL

/* Scroll the visible viewport left / right by one column.
 * Called from the keyboard handler in nterm.c. */
void ansi_viewport_left(void);
void ansi_viewport_right(void);

/* Jump viewport left / right by 8 columns (Page Back / Page Fwd keys). */
void ansi_viewport_page_left(void);
void ansi_viewport_page_right(void);

#else /* VDP_80COL */

/* Cycle through the built-in text colour table (Ctrl+T).
 * Applies immediately to the global VDP colour register. */
void ansi_cycle_colour(void);

#endif /* VDP_G2COL */

/* Show the key-reference help overlay and block until any key is pressed.
 * Triggered by the NABU SYM key (0xE8) in both builds.
 * G2:    restores screen via ansi_render_viewport() on dismiss.
 * 80-col: restores VRAM from _vdp_textBuffer on dismiss. */
void ansi_show_help(void);

#endif /* ANSI_H */
