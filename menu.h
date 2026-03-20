/*
 * menu.h -- Host/preset selection menu
 *
 * Included into nterm.c via  #include "menu.c"
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h here; included by nterm.c.
 */

#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum hostname length (not including null terminator) */
#define MENU_HOST_MAX  64

/*
 * Show the preset selection menu and wait for the user to pick a host.
 *
 *   host_out     : output buffer, must be at least MENU_HOST_MAX+1 bytes
 *   host_len_out : set to the hostname length (without null)
 *   port_out     : set to the selected port number
 *
 * Presets (up to 5) are loaded from / saved to NBTERM.CFG on the IA
 * file store.  Blocks until the user selects a valid, non-empty preset.
 */
/* Returns true if a preset was selected (connect); false if user quit. */
bool menu_run(uint8_t *host_out, uint8_t *host_len_out, uint16_t *port_out);

#endif /* MENU_H */
