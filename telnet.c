/*
 * telnet.c -- Minimal telnet IAC negotiation (RFC 854)
 *
 * Included into nterm.c via  #include "telnet.c".
 * Do NOT include NABU-LIB.h here; it is already included by nterm.c
 * before this file is pulled in.
 */

#include "telnet.h"

/* -----------------------------------------------------------------------
 * Parser states
 * --------------------------------------------------------------------- */
#define _ST_NORMAL  0   /* normal data stream          */
#define _ST_IAC     1   /* seen 0xFF, awaiting command */
#define _ST_CMD     2   /* seen WILL/WONT/DO/DONT      */
#define _ST_SB      3   /* inside subnegotiation       */
#define _ST_SB_IAC  4   /* saw IAC inside subneg       */

static uint8_t _tn_state = _ST_NORMAL;
static uint8_t _tn_cmd   = 0;

bool tn_server_echo = false;

/* -----------------------------------------------------------------------
 * Internal: send a 3-byte IAC reply
 * --------------------------------------------------------------------- */
static void _tn_reply(uint8_t cmd, uint8_t opt, uint8_t tcpHandle)
{
    uint8_t buf[3];
    buf[0] = TN_IAC;
    buf[1] = cmd;
    buf[2] = opt;
    rn_TCPHandleWrite(tcpHandle, 0, 3, buf);
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
void tn_init(void)
{
    _tn_state      = _ST_NORMAL;
    _tn_cmd        = 0;
    tn_server_echo = false;
}

bool tn_feed(uint8_t b, uint8_t tcpHandle)
{
    switch (_tn_state) {

    case _ST_NORMAL:
        if (b == TN_IAC) {
            _tn_state = _ST_IAC;
            return false;
        }
        return true;

    case _ST_IAC:
        if (b == TN_IAC) {
            /* Escaped 0xFF in data stream — treat as literal byte */
            _tn_state = _ST_NORMAL;
            return true;
        }
        if (b == TN_SB) {
            _tn_state = _ST_SB;
            return false;
        }
        if (b == TN_WILL || b == TN_WONT ||
            b == TN_DO   || b == TN_DONT) {
            _tn_cmd   = b;
            _tn_state = _ST_CMD;
            return false;
        }
        /* NOP, GA, DM, BRK, AYT, etc. — silently discard */
        _tn_state = _ST_NORMAL;
        return false;

    case _ST_CMD:
        _tn_state = _ST_NORMAL;
        switch (_tn_cmd) {
        case TN_WILL:
            /* Server offers to enable an option */
            if (b == TN_OPT_ECHO) {
                tn_server_echo = true;          /* server will echo all input */
                _tn_reply(TN_DO, b, tcpHandle);
            } else if (b == TN_OPT_SGA) {
                _tn_reply(TN_DO, b, tcpHandle); /* accept full-duplex mode */
            } else {
                _tn_reply(TN_DONT, b, tcpHandle); /* refuse everything else */
            }
            break;
        case TN_DO:
            /* Server asks us to enable an option — refuse all */
            _tn_reply(TN_WONT, b, tcpHandle);
            break;
        case TN_WONT:
            /* Server disabling — track ECHO state, no reply needed */
            if (b == TN_OPT_ECHO) {
                tn_server_echo = false;
            }
            break;
        case TN_DONT:
            /* No reply required */
            break;
        }
        return false;

    case _ST_SB:
        /* Skip subnegotiation bytes until IAC SE */
        if (b == TN_IAC) {
            _tn_state = _ST_SB_IAC;
        }
        return false;

    case _ST_SB_IAC:
        _tn_state = (b == TN_SE) ? _ST_NORMAL : _ST_SB;
        return false;

    default:
        _tn_state = _ST_NORMAL;
        return false;
    }
}
