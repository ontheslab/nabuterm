/*
 * ansi.h -- ANSI/VT100 escape sequence parser interface
 */

#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>
#include <stdbool.h>

/* Reset all parser state and colour attributes.
 * Call once before each new connection. */
void ansi_reset(void);

/* Feed one display byte (post-telnet-strip) into the parser.
 * Handles printable chars, control chars, and ESC sequences.
 * Replaces term_putc — this is the single display entry point. */
void ansi_feed(uint8_t c);

#endif /* ANSI_H */
