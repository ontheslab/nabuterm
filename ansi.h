/*
 * ansi.h -- ANSI/VT100 escape sequence parser interface
 */

#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>
#include <stdbool.h>

/* Reset all parser state and colour attributes.
 * In G2 mode also clears the virtual buffer and resets pattern colours.
 * Call once before each new connection. */
void ansi_reset(void);

/* Feed one display byte (post-telnet-strip) into the parser.
 * Handles printable chars, control chars, and ESC sequences. */
void ansi_feed(uint8_t c);

#ifdef VDP_G2COL

/* Re-render the full 32x24 VDP viewport from the virtual buffer.
 * Called after viewport scroll or full-screen operations. */
void ansi_render_viewport(void);

/* Scroll the visible viewport left / right by one column.
 * Called from the keyboard handler in nterm.c. */
void ansi_viewport_left(void);
void ansi_viewport_right(void);

#endif /* VDP_G2COL */

#endif /* ANSI_H */
