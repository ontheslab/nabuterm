/*
 * telnet.h -- Minimal telnet IAC negotiation (RFC 854)
 */

#ifndef TELNET_H
#define TELNET_H

#include <stdint.h>
#include <stdbool.h>

/* Telnet command bytes */
#define TN_IAC   ((uint8_t)0xFF)
#define TN_SE    ((uint8_t)0xF0)
#define TN_SB    ((uint8_t)0xFA)
#define TN_WILL  ((uint8_t)0xFB)
#define TN_WONT  ((uint8_t)0xFC)
#define TN_DO    ((uint8_t)0xFD)
#define TN_DONT  ((uint8_t)0xFE)

/* Option codes we care about */
#define TN_OPT_ECHO  ((uint8_t)1)
#define TN_OPT_SGA   ((uint8_t)3)

/*
 * Set to true when the server has said WILL ECHO (it echoes our input).
 * Set to false when server says WONT ECHO (we must echo locally).
 */
extern bool tn_server_echo;

/* Reset state machine. Call once before each new connection. */
void tn_init(void);

/*
 * Feed one byte from the TCP receive stream.
 *   tcpHandle : active TCP connection used to send negotiation replies
 * Returns true  if the byte is normal display data (pass to terminal).
 * Returns false if the byte was consumed as part of a telnet command.
 */
bool tn_feed(uint8_t b, uint8_t tcpHandle);

#endif /* TELNET_H */
