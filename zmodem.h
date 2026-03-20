/*
 * zmodem.h -- ZModem receive interface
 *
 * Included into nterm.c via  #include "zmodem.c"
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h here.
 */

#ifndef ZMODEM_H
#define ZMODEM_H

#include <stdint.h>

/* Maximum filename length accepted from sender */
#define ZM_FNAME_MAX  32

/*
 * Receive a ZModem file transfer session.
 * Called from main loop when zm_detect() fires.
 * pre/prelen: bytes already read from TCP into _rx after the detection
 *             point -- passed in so they are not lost.
 * Blocks until transfer complete, aborted, or timed out.
 * Received files are written to the IA file store.
 */
void zmodem_receive(uint8_t tcpHandle, uint8_t *pre, uint8_t prelen);

#endif /* ZMODEM_H */
