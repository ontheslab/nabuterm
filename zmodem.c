/*
 * zmodem.c -- ZModem receive implementation
 *
 * Included into nterm.c via  #include "zmodem.c"
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h here.
 *
 * Implements receive-only ZModem over the IA TCP connection.
 * Sends ZHEX frames; accepts ZHEX, ZBIN, and CRC-32 binary headers if seen.
 * CANFC32 is omitted from the ZRINIT flags -- CRC-16 is used throughout.
 * Received files are written to the IA file store (append-only).
 *
 * Protocol reference: zmp by Wayne Warthen (github.com/mecparts/zmp) --
 * a CP/M ZModem implementation in C for Z80, used as the structural
 * reference for the overall state machine and frame layout.
 * AmiExpress source (github.com/dmcoles/AmiExpress, zmodem.e) was used
 * to understand sender-specific behaviour during AmiExpress debugging.
 */

#include "zmodem.h"

/* -----------------------------------------------------------------------
 * Protocol constants
 * --------------------------------------------------------------------- */
#define _ZP    0x2A   /* ZPAD  '*'                    */
#define _ZD    0x18   /* ZDLE                         */
#define _ZHX   0x42   /* 'B' -- ZHEX header marker    */
#define _ZBN   0x41   /* 'A' -- ZBIN header marker    */

#define _ZRQINIT  0
#define _ZRINIT   1
#define _ZSINIT   2
#define _ZACK     3
#define _ZFILE    4
#define _ZSKIP    5
#define _ZNAK     6
#define _ZABORT   7
#define _ZFIN     8
#define _ZRPOS    9
#define _ZDATA   10
#define _ZEOF    11
#define _ZFERR   12

/* Sub-packet terminators (follow ZDLE in byte stream) */
#define _ZCRCE  0x68   /* end, header packet follows  */
#define _ZCRCG  0x69   /* go on, no ACK needed        */
#define _ZCRCQ  0x6A   /* go on, ZACK expected        */
#define _ZCRCW  0x6B   /* end of frame, ZACK expected */
#define _ZRUB0  0x6C   /* escaped 0x7F               */
#define _ZRUB1  0x6D   /* escaped 0xFF               */

/* Capability flags sent in the ZRINIT frame */
#define _CANFDX   0x01
#define _CANOVIO  0x02
/* NOTE: CANFC32 (0x20) deliberately omitted -- forces CRC-16 */

/* Timeout: rn_TCPHandleRead poll iterations before giving up.
 * Each poll is one HCCA round-trip (~540μs real time at 111Kbps).
 * 20000 × 540μs ≈ 10 seconds.  If this is hit _zdbg_got stays 0. */
#define _ZTIMEOUT  20000u

/* -----------------------------------------------------------------------
 * Module state -- all static
 * --------------------------------------------------------------------- */
static uint8_t  _zhandle;           /* active TCP handle              */

static uint8_t  _zrx[128];          /* TCP receive buffer             */
static uint8_t  _zrxpos;            /* read index into _zrx           */
static uint8_t  _zrxlen;            /* valid bytes in _zrx            */

static uint8_t  _ztx[24];           /* TX staging (ZHEX frame = 20 B) */
static uint8_t  _ztxlen;

static uint8_t  _zwr[1024];         /* sub-packet buffer -- holds one full
                                     * sub-packet (up to 1024 bytes) in RAM
                                     * so the file write happens AFTER CRC
                                     * verification, not mid-reception     */
static uint16_t _zwrlen;

static uint8_t  _zhdr[4];           /* last received header data[0-3] */

static uint8_t  _zfname[ZM_FNAME_MAX + 1];
static uint32_t _zfpos;             /* bytes written to current file  */
static uint32_t _zfsize;            /* file size from ZFILE metadata (0 = unknown) */
static uint8_t  _zcrc32;            /* 1 = sender using CRC-32 frames */

static uint8_t  _zdat_row;          /* VDP row of the ZDATA progress line */

static uint16_t _zcrc[256];         /* CRC-16 CCITT lookup table      */

/* -----------------------------------------------------------------------
 * CRC-16 CCITT (polynomial 0x1021, initial value 0x0000)
 * Standard table-driven approach; table built once per session.
 * Polynomial and init value from the ZModem spec (matches zmp).
 * --------------------------------------------------------------------- */
static void _crc_init(void)
{
    uint16_t i, c;
    uint8_t  j;
    for (i = 0; i < 256u; i++) {
        c = (uint16_t)(i << 8);
        for (j = 0; j < 8u; j++) {
            if (c & 0x8000u)
                c = (uint16_t)((uint16_t)(c << 1) ^ 0x1021u);
            else
                c = (uint16_t)(c << 1);
        }
        _zcrc[i] = c;
    }
}

static uint16_t _crc16(uint16_t crc, uint8_t b)
{
    return (uint16_t)(_zcrc[(uint8_t)(crc >> 8) ^ b] ^ (uint16_t)(crc << 8));
}

/* CRC-32 (IEEE 802.3 / PKZIP reflected polynomial 0xEDB88320).
 * Used only when the sender switches to CRC-32 -- not the normal path. */
static uint32_t _crc32b(uint32_t crc, uint8_t b)
{
    uint8_t i;

    crc ^= (uint32_t)b;
    for (i = 0u; i < 8u; i++) {
        if (crc & 1u)
            crc = (crc >> 1) ^ 0xEDB88320uL;
        else
            crc >>= 1;
    }
    return crc;
}

/* -----------------------------------------------------------------------
 * Low-level TCP I/O
 * --------------------------------------------------------------------- */

/* Minimal status kept for user-visible error reporting */
static int32_t  _zdbg_got;    /* last return from rn_TCPHandleRead */
static uint8_t  _zdbg_htype;  /* header type byte read after ZDLE  */
static uint8_t  _zdbg_hfail;  /* _rxhdr failure code               */
static uint16_t _zdbg_crc_c;  /* CRC computed on mismatch          */
static uint16_t _zdbg_crc_r;  /* CRC received on mismatch          */
static uint8_t  _zdbg_rxfail; /* _rxdata() failure code            */

/* Read one raw byte from the TCP stream.
 * Refills _zrx from TCP when the internal buffer is empty.
 * Returns the byte (0-255 as int16_t), or -1 on timeout or disconnect.
 *
 * Polls via rn_TCPHandleRead so each call is one HCCA round-trip.
 * Both disconnect and timeout return -1; the caller can tell them apart
 * by checking _zdbg_got (negative = disconnect, zero = timeout).
 */
static int16_t _rxb_raw(void)
{
    uint32_t retry;
    int32_t  got;

    if (_zrxpos < _zrxlen)
        return (int16_t)_zrx[_zrxpos++];

    for (retry = 0u; retry < _ZTIMEOUT; retry++) {
        got = rn_TCPHandleRead(_zhandle, _zrx, 0, 128u);
        _zdbg_got = got;
        if (got < 0) return -1;    /* TCP disconnected (-1) */
        if (got > 0) {
            _zrxlen = (uint8_t)got;
            _zrxpos = 0;
            return (int16_t)_zrx[_zrxpos++];
        }
    }
    return -1;  /* timeout -- _zdbg_got stays 0 */
}

/* Read one data byte for ZMODEM, passing the stream through the normal telnet
 * parser so doubled IAC (0xFF 0xFF) collapses back to a literal 0xFF and any
 * stray negotiations are consumed exactly as they are in the terminal path. */
static int16_t _rxb(void)
{
    int16_t b;

    for (;;) {
        b = _rxb_raw();
        if (b < 0) return -1;
        if (tn_feed((uint8_t)b, _zhandle))
            return b;
    }
}

static void _txflush(void)
{
    if (_ztxlen > 0u) {
        rn_TCPHandleWrite(_zhandle, 0, _ztxlen, _ztx);
        _ztxlen = 0;
    }
}

static void _txb(uint8_t b)
{
    if (_ztxlen < (uint8_t)sizeof(_ztx))
        _ztx[_ztxlen++] = b;
}

/* -----------------------------------------------------------------------
 * Hex helpers
 * --------------------------------------------------------------------- */
static uint8_t _nib(uint8_t n)
{
    n &= 0x0Fu;
    return (uint8_t)(n < 10u ? (uint8_t)('0' + n) : (uint8_t)('a' + n - 10u));
}

static int16_t _unhex(uint8_t c)
{
    if (c >= '0' && c <= '9') return (int16_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (int16_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int16_t)(c - 'A' + 10);
    return -1;
}

/* -----------------------------------------------------------------------
 * Display helpers
 * --------------------------------------------------------------------- */
static void _znl(void)
{
    vdp_newLine();
    vdp_setCursor2(0, vdp_cursor.y);
}

/* Print uint32_t as decimal (no leading zeros) */
static void _zprintdec32(uint32_t v)
{
    uint32_t t;
    uint8_t  d;
    uint8_t  leading;

    if (v == 0u) { vdp_write('0'); return; }
    leading = 1u;

    t = 1000000000uL; d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 100000000uL;  d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 10000000uL;   d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 1000000uL;    d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 100000uL;     d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 10000uL;      d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 1000uL;       d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 100uL;        d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    t = 10uL;         d = 0u; while (v >= t) { v -= t; d++; }
    if (d || !leading) { vdp_write((uint8_t)('0' + d)); leading = 0u; }
    vdp_write((uint8_t)('0' + (uint8_t)v));
}

/* -----------------------------------------------------------------------
 * ZDLE-decoded receive
 *
 * Returns 0x000..0x0FF  : normal decoded byte value
 *         0x100          : _ZCRCE terminator
 *         0x101          : _ZCRCG terminator
 *         0x102          : _ZCRCQ terminator
 *         0x103          : _ZCRCW terminator
 *         -1             : timeout or disconnect
 *
 * XON (0x11), XOFF (0x13) and high-bit variants (0x91, 0x93) are
 * discarded in TWO places:
 *   1. Before the ZDLE check -- handles standalone flow-control bytes
 *      injected anywhere in the stream (e.g. after ZHEX headers).
 *   2. After ZDLE, before reading the escape code -- handles XON/XOFF
 *      injected between ZDLE and the escape byte (e.g. AmiExpress sends
 *      raw XON after every ZCRCW sub-packet; if that XON arrives in the
 *      TCP buffer immediately after the ZDLE terminator prefix, it is
 *      mistaken for an escape code and decoded as a spurious data byte,
 *      causing _rxdata() to overflow at byte 1024).
 * --------------------------------------------------------------------- */
static int16_t _rxzd(void)
{
    int16_t b;
    for (;;) {
        b = _rxb();
        if (b < 0) return -1;
        /* Discard XON/XOFF flow-control bytes */
        if ((uint8_t)b == 0x11u || (uint8_t)b == 0x13u ||
            (uint8_t)b == 0x91u || (uint8_t)b == 0x93u)
            continue;
        if ((uint8_t)b != _ZD) return b;
        /* ZDLE seen: read the escape code, skipping any XON/XOFF between */
        for (;;) {
            b = _rxb();
            if (b < 0) return -1;
            if ((uint8_t)b == 0x11u || (uint8_t)b == 0x13u ||
                (uint8_t)b == 0x91u || (uint8_t)b == 0x93u)
                continue;
            break;
        }
        switch ((uint8_t)b) {
        case _ZCRCE: return (int16_t)0x100;
        case _ZCRCG: return (int16_t)0x101;
        case _ZCRCQ: return (int16_t)0x102;
        case _ZCRCW: return (int16_t)0x103;
        case _ZRUB0: return (int16_t)0x7F;
        case _ZRUB1: return (int16_t)0xFF;
        default:     return (int16_t)((uint8_t)b ^ 0x40u);
        }
    }
}

static uint8_t _rx_crc16(uint16_t *out)
{
    int16_t  b;
    uint16_t crc;

    b = _rxzd();
    if (b < 0 || b > (int16_t)0xFF) return 0u;
    crc = (uint16_t)(((uint16_t)(uint8_t)b) << 8);

    b = _rxzd();
    if (b < 0 || b > (int16_t)0xFF) return 0u;
    crc |= (uint8_t)b;

    *out = crc;
    return 1u;
}

static uint8_t _rx_crc32(uint32_t *out)
{
    int16_t  b;
    uint32_t crc32;
    uint8_t  shift;

    crc32 = 0uL;
    for (shift = 0u; shift < 32u; shift += 8u) {
        b = _rxzd();
        if (b < 0 || b > (int16_t)0xFF) return 0u;
        crc32 |= (uint32_t)(uint8_t)b << shift;
    }

    *out = crc32;
    return 1u;
}

/* -----------------------------------------------------------------------
 * _txzhex -- transmit one ZHEX header frame
 *
 * Wire format: ** ZDLE 'B' TT D0D0 D1D1 D2D2 D3D3 CCCC CR LF [XON]
 * (TT and each Dx are 2 hex chars; CCCC = 4 hex chars CRC-16)
 * CRC covers: type, d0, d1, d2, d3 (5 bytes, init=0).
 * ZMODEM appends XON to all HEX headers except ZACK and ZFIN.
 * --------------------------------------------------------------------- */
static void _txzhex(uint8_t type,
                    uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3)
{
    uint16_t crc;
    uint8_t  add_xon;

    add_xon      = (uint8_t)(type != _ZACK && type != _ZFIN);

    _ztxlen = 0;
    _txb(_ZP); _txb(_ZP); _txb(_ZD); _txb(_ZHX);
    _txb(_nib(type >> 4)); _txb(_nib(type));
    _txb(_nib(d0 >> 4)); _txb(_nib(d0));
    _txb(_nib(d1 >> 4)); _txb(_nib(d1));
    _txb(_nib(d2 >> 4)); _txb(_nib(d2));
    _txb(_nib(d3 >> 4)); _txb(_nib(d3));
    crc = _crc16(0u,   type);
    crc = _crc16(crc,  d0);
    crc = _crc16(crc,  d1);
    crc = _crc16(crc,  d2);
    crc = _crc16(crc,  d3);
    _txb(_nib((uint8_t)(crc >> 12)));
    _txb(_nib((uint8_t)(crc >>  8)));
    _txb(_nib((uint8_t)(crc >>  4)));
    _txb(_nib((uint8_t)(crc      )));
    _txb('\r'); _txb('\n');
    if (add_xon)
        _txb(0x11u);
    _txflush();
}

static void _txpos(uint8_t type, uint32_t pos)
{
    uint8_t *pp;

    pp = (uint8_t *)&pos;
    _txzhex(type, pp[0], pp[1], pp[2], pp[3]);
}

/* -----------------------------------------------------------------------
 * _rxhdr -- receive one header frame (ZHEX, ZBIN, or ZBIN32)
 *
 * Synchronises on the ZPAD [ZPAD] ZDLE prefix before reading the frame.
 * Requiring the ZPAD(s) stops false relock on ZDLE bytes that appear
 * inside data sub-packets (e.g. ZDLE ZCRCW terminator sequences).
 *
 * After the prefix: reads the frame style byte, 5 data bytes (type +
 * ZP0..ZP3), and the CRC (2 bytes CRC-16 or 4 bytes CRC-32).
 * Stores the 4 data bytes in _zhdr[0..3].
 * Returns the frame type (0-12), or 0xFF on error / timeout.
 * --------------------------------------------------------------------- */
static uint8_t _rxhdr(void)
{
    int16_t  b;
    uint8_t  htype;
    uint8_t  d[5];       /* d[0]=type, d[1-4]=ZP0-ZP3 */
    uint16_t crc, rcrc;
    uint32_t crc32, rcrc32;
    uint8_t  i;
    int16_t  hi, lo;
    uint8_t  skip_crc;   /* 1 = CRC-32 fallback applied, skip common check */

    /* Synchronize on ZPAD[*] ZDLE.
     * After a bad header, grabbing the first bare ZDLE can lock onto a
     * sub-packet terminator (e.g. ZDLE ZCRCW) instead of the next frame. */
    _zdbg_htype = 0;
    _zdbg_hfail = '?';
    skip_crc    = 0;
    rcrc        = 0u;
    crc32       = 0uL;
    rcrc32      = 0uL;
    for (;;) {
        b = _rxb();
        if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
        if ((uint8_t)b == 0x11u || (uint8_t)b == 0x13u ||
            (uint8_t)b == 0x91u || (uint8_t)b == 0x93u)
            continue;
        if ((uint8_t)b != _ZP)
            continue;

        b = _rxb();
        if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
        while ((uint8_t)b == 0x11u || (uint8_t)b == 0x13u ||
               (uint8_t)b == 0x91u || (uint8_t)b == 0x93u) {
            b = _rxb();
            if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
        }
        if ((uint8_t)b == _ZP) {
            b = _rxb();
            if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
            while ((uint8_t)b == 0x11u || (uint8_t)b == 0x13u ||
                   (uint8_t)b == 0x91u || (uint8_t)b == 0x93u) {
                b = _rxb();
                if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
            }
        }
        if ((uint8_t)b == _ZD)
            break;
    }

    /* Read header type byte: 'B' (ZHEX), 'A' (ZBIN), 'C' (ZBIN32) */
    b = _rxb();
    if (b < 0) { _zdbg_hfail = 'T'; return 0xFF; }
    htype = (uint8_t)b;
    _zdbg_htype = htype;

    if (htype == _ZHX) {
        /* ZHEX: 5 bytes as 10 hex chars, then CRC as 4 hex chars, then CR LF */
        for (i = 0u; i < 5u; i++) {
            hi = _rxb(); if (hi < 0) { _zdbg_hfail = 'X'; return 0xFF; }
            lo = _rxb(); if (lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
            hi = _unhex((uint8_t)hi);
            lo = _unhex((uint8_t)lo);
            if (hi < 0 || lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
            d[i] = (uint8_t)((uint8_t)((uint8_t)hi << 4) | (uint8_t)lo);
        }
        /* CRC: 4 hex chars -> 2 bytes */
        hi = _rxb(); if (hi < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        lo = _rxb(); if (lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        hi = _unhex((uint8_t)hi); lo = _unhex((uint8_t)lo);
        if (hi < 0 || lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        rcrc = (uint16_t)((uint16_t)((uint8_t)((uint8_t)hi << 4) | (uint8_t)lo) << 8);
        hi = _rxb(); if (hi < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        lo = _rxb(); if (lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        hi = _unhex((uint8_t)hi); lo = _unhex((uint8_t)lo);
        if (hi < 0 || lo < 0) { _zdbg_hfail = 'X'; return 0xFF; }
        rcrc |= (uint16_t)((uint8_t)((uint8_t)hi << 4) | (uint8_t)lo);
        /* Consume CR LF (XON if present is skipped next time through drain) */
        _rxb(); _rxb();

    } else if (htype == _ZBN) {
        /* ZBIN: 5 ZDLE-decoded bytes, then 2 ZDLE-decoded CRC bytes.
         * Some senders use CRC-32 (4 bytes, little-endian) with the ZBIN
         * 'A' marker; if CRC-16 fails but the type byte is valid, consume
         * the extra 2 bytes and verify CRC-32 properly. */
        for (i = 0u; i < 5u; i++) {
            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            d[i] = (uint8_t)b;
        }
        {
            uint8_t c0, c1, c2, c3;

            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            c0 = (uint8_t)b;
            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            c1 = (uint8_t)b;
            rcrc = (uint16_t)((uint16_t)c0 << 8);
            rcrc |= c1;

        crc = 0u;
        for (i = 0u; i < 5u; i++) crc = _crc16(crc, d[i]);
        if (crc != rcrc) {
            _zdbg_crc_c = crc;
            _zdbg_crc_r = rcrc;
            if (d[0] <= (uint8_t)_ZFERR) {
                    b = _rxzd();
                    if (b < 0 || b > (int16_t)0xFF) {
                        _zdbg_hfail = 'B';
                        return 0xFF;
                    }
                    c2 = (uint8_t)b;

                    b = _rxzd();
                    if (b < 0 || b > (int16_t)0xFF) {
                        _zdbg_hfail = 'B';
                        return 0xFF;
                    }
                    c3 = (uint8_t)b;

                    rcrc32  = (uint32_t)c0;
                    rcrc32 |= (uint32_t)c1 << 8;
                    rcrc32 |= (uint32_t)c2 << 16;
                    rcrc32 |= (uint32_t)c3 << 24;

                    crc32 = 0xFFFFFFFFuL;
                    for (i = 0u; i < 5u; i++)
                        crc32 = _crc32b(crc32, d[i]);
                    crc32 = ~crc32;

                    if (crc32 == rcrc32) {
                        _zdbg_hfail = 'K';
                        _zcrc32     = 1;
                        skip_crc    = 1;
                    } else {
                        _zdbg_hfail = 'C';
                        return 0xFF;
                    }
            } else {
                _zdbg_hfail = 'C';
                return 0xFF;
            }
        }
        }

    } else if (htype == 0x43u) {
        /* ZBIN32 ('C'): 5 ZDLE-decoded bytes, then 4 ZDLE-decoded CRC-32
         * bytes in little-endian order. */
        for (i = 0u; i < 5u; i++) {
            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            d[i] = (uint8_t)b;
        }
        if (!_rx_crc32(&rcrc32)) { _zdbg_hfail = 'B'; return 0xFF; }
        crc32 = 0xFFFFFFFFuL;
        for (i = 0u; i < 5u; i++)
            crc32 = _crc32b(crc32, d[i]);
        crc32 = ~crc32;
        if (crc32 != rcrc32) {
            _zdbg_hfail = 'C';
            return 0xFF;
        }
        if (d[0] > (uint8_t)_ZFERR) { _zdbg_hfail = 'U'; return 0xFF; }
        _zdbg_hfail = 'K';
        _zcrc32     = 1;
        skip_crc    = 1;

    } else {
        _zdbg_hfail = 'U';
        return 0xFF;   /* unknown header type */
    }

    /* Verify CRC-16 over type + 4 data bytes (skipped for CRC-32 fallback) */
    if (!skip_crc) {
        crc = 0u;
        for (i = 0u; i < 5u; i++) crc = _crc16(crc, d[i]);
        if (crc != rcrc) {
            _zdbg_hfail = 'C';
            _zdbg_crc_c = crc;
            _zdbg_crc_r = rcrc;
            return 0xFF;
        }
    }

    _zhdr[0] = d[1]; _zhdr[1] = d[2];
    _zhdr[2] = d[3]; _zhdr[3] = d[4];
    return d[0];
}

/* -----------------------------------------------------------------------
 * _drain_subpkt -- discard a data sub-packet and its trailing CRC bytes
 * Used to consume the ZSINIT attention string without processing it.
 * --------------------------------------------------------------------- */
static void _drain_subpkt(void)
{
    int16_t b;

    for (;;) {
        b = _rxzd();
        if (b < 0 || b > (int16_t)0xFF) break;
    }
    if (_zcrc32) {
        _rxzd();
        _rxzd();
        _rxzd();
        _rxzd();
    } else {
        _rxzd();
        _rxzd();
    }
}

/* -----------------------------------------------------------------------
 * _wrflush -- flush file write buffer to IA store
 * --------------------------------------------------------------------- */
static void _wrflush(uint8_t fh)
{
    if (_zwrlen > 0u) {
        rn_fileHandleAppend(fh, 0, _zwrlen, _zwr);
        _zwrlen = 0;
    }
}

/* -----------------------------------------------------------------------
 * _rxzfile -- read ZFILE data sub-packet
 *
 * Extracts the filename into _zfname and the file size into _zfsize.
 * Sub-packet format after the header: null-terminated filename, then
 * space-separated decimal fields: size mtime mode misc.
 * We read the filename up to its null, then the decimal digits of the
 * size field (stopping at the first space or null after the filename).
 * Returns 0 on success, 1 on CRC error or timeout.
 * --------------------------------------------------------------------- */
static uint8_t _rxzfile(void)
{
    int16_t  b;
    uint8_t  fi, fname_done, fsize_done;
    uint16_t crc, rcrc;
    uint32_t crc32, rcrc32;
    uint8_t  term;

    fi          = 0u;
    fname_done  = 0u;
    fsize_done  = 0u;
    crc         = 0u;
    crc32       = 0xFFFFFFFFuL;
    _zfname[0]  = 0;
    _zfsize     = 0u;

    for (;;) {
        b = _rxzd();
        if (b < 0) return 1;

        if (b > (int16_t)0xFF) {
            /* Sub-packet terminator */
            term = (uint8_t)((uint16_t)b - 0x100u + _ZCRCE);
            crc  = _crc16(crc, term);
            crc32 = _crc32b(crc32, term);
            if (_zcrc32) {
                crc32 = ~crc32;
                if (!_rx_crc32(&rcrc32) || crc32 != rcrc32)
                    return 1;
            } else {
                if (!_rx_crc16(&rcrc) || crc != rcrc) return 1;
            }
            break;
        }

        crc = _crc16(crc, (uint8_t)b);
        crc32 = _crc32b(crc32, (uint8_t)b);

        if (!fname_done) {
            if ((uint8_t)b == 0u) {
                /* null terminator of filename */
                _zfname[fi] = 0;
                fname_done = 1u;
            } else if (fi < (uint8_t)ZM_FNAME_MAX) {
                _zfname[fi++] = (uint8_t)b;
            }
        } else if (!fsize_done) {
            /* First field after filename null is the decimal file size */
            if ((uint8_t)b >= '0' && (uint8_t)b <= '9') {
                _zfsize = _zfsize * 10u + (uint32_t)((uint8_t)b - '0');
            } else {
                fsize_done = 1u;  /* space, null, or end of size field */
            }
        }
    }

    if (!fname_done)
        _zfname[fi] = 0;

    return 0;
}

/* -----------------------------------------------------------------------
 * _rxdata -- receive one data sub-packet into _zwr[], verify CRC.
 *
 * Data is buffered in RAM only -- no file write happens here.
 * The caller writes _zwr to the file AFTER this function returns
 * success, so no HCCA file traffic occurs while bytes are arriving.
 *
 * Returns the terminator byte (_ZCRCE/_ZCRCG/_ZCRCQ/_ZCRCW) on success,
 * 0xFE if the sub-packet exceeded the 1024-byte buffer but was drained
 * cleanly to the terminator (caller should ZRPOS; do not close file),
 * or 0xFF on timeout, CRC error, or disconnect.
 * --------------------------------------------------------------------- */
static uint8_t _rxdata(void)
{
    int16_t  b;
    uint8_t  dat, term;
    uint16_t crc, rcrc;
    uint32_t crc32, rcrc32;

    crc = 0u;
    crc32 = 0xFFFFFFFFuL;
    _zdbg_rxfail = '?';

    for (;;) {
        b = _rxzd();
        if (b < 0) { _zdbg_rxfail = 'T'; return 0xFF; }

        if (b > (int16_t)0xFF) {
            /* Terminator: include terminator byte itself in CRC */
            term = (uint8_t)((uint16_t)b - 0x100u + _ZCRCE);
            crc  = _crc16(crc, term);
            crc32 = _crc32b(crc32, term);
            if (_zcrc32) {
                crc32 = ~crc32;
                if (!_rx_crc32(&rcrc32) || crc32 != rcrc32) {
                    _zdbg_rxfail = 'K'; return 0xFF;
                }
            } else {
                if (!_rx_crc16(&rcrc)) {
                    _zdbg_rxfail = 'K'; return 0xFF;
                }
                if (crc != rcrc) {
                    _zdbg_rxfail = 'C';
                    _zdbg_crc_c  = crc;
                    _zdbg_crc_r  = rcrc;
                    return 0xFF;
                }
            }
            return term;
        }

        /* Sub-packet exceeds the 1024-byte buffer.
         * Drain the remainder to re-sync the stream, then return 0xFE.
         * The caller sends ZRPOS; AmiExpress halves its block_size on
         * each ZRPOS and will eventually reach <= 1024 bytes/sub-packet.
         * Do NOT close the file -- the data written so far is still good. */
        if (_zwrlen >= 1024u) {
            _zdbg_rxfail = 'O';
            for (;;) {
                b = _rxzd();
                if (b < 0) return 0xFF;
                if (b > (int16_t)0xFF) {
                    /* Terminator found: drain CRC bytes and return */
                    if (_zcrc32) {
                        if (!_rx_crc32(&rcrc32))
                            return 0xFF;
                    } else {
                        if (_rxzd() < 0 || _rxzd() < 0) return 0xFF;
                    }
                    return 0xFE;  /* drained cleanly -- caller sends ZRPOS */
                }
                /* discard data byte; caller will restore _zfpos */
            }
        }
        dat = (uint8_t)b;
        crc = _crc16(crc, dat);
        crc32 = _crc32b(crc32, dat);
        _zwr[_zwrlen++] = dat;
        _zfpos++;
    }
}

/* -----------------------------------------------------------------------
 * zmodem_receive -- public entry point
 *
 * Called from main loop when zm_detect() fires (ZRQINIT seen).
 * Blocks until transfer session complete, aborted, or timed out.
 * Writes received files to the IA file store.
 * --------------------------------------------------------------------- */
void zmodem_receive(uint8_t tcpHandle, uint8_t *pre, uint8_t prelen)
{
    uint8_t  ftype;
    uint8_t  fh;
    uint8_t  term;
    uint8_t  done;
    uint8_t  fname_len;
    uint8_t  ttype;      /* last sub-packet terminator: e/g/q/w/! */
    uint8_t  hdr_fails;  /* consecutive recoverable header failures */
    int32_t  ds;
    uint32_t eof_pos;

    _zhandle   = tcpHandle;
    /* Seed the receive buffer with any bytes already pulled from TCP
     * by the main loop's rn_TCPHandleRead() call -- without this they
     * would be lost since rn_TCPHandleRead() already consumed them. */
    if (prelen > 0u) {
        uint8_t k;
        for (k = 0u; k < prelen; k++)
            _zrx[k] = pre[k];
        _zrxpos = 0;
        _zrxlen = prelen;
    } else {
        _zrxpos = 0;
        _zrxlen = 0;
    }
    _ztxlen    = 0;
    _zwrlen    = 0;
    _zfpos     = 0u;
    _zfname[0] = 0;
    fh         = 0xFF;
    done       = 0;
    hdr_fails  = 0u;

    _zdbg_got    = 0;
    _zdbg_htype  = 0;
    _zdbg_hfail  = '?';
    _zdbg_crc_c  = 0u;
    _zdbg_crc_r  = 0u;
    _zdbg_rxfail = '?';
    _zcrc32      = 0;
    _zdat_row    = 0xFF;
    _zfsize      = 0u;
    _crc_init();

    vdp_clearScreen();
    vdp_setCursor2(0, 0);
    vdp_setTextColor(VDP_CYAN, VDP_BLACK);
    vdp_print((uint8_t *)"ZModem Receive");
    _znl();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);

    /* d0=Rxbuflen low, d1=Rxbuflen high (1024), d2=0, d3=ZF0 flags.
     * Rxbuflen = d0 + d1*256.  lrzsz rejects Rxbuflen < 32 and falls
     * back to 8192, so flags must be in d3 (ZF0), not d0. */
    _txzhex(_ZRINIT, 0, 4u, 0, _CANFDX | _CANOVIO);

    /* Check TCP state after sending ZRINIT */
    ds = rn_TCPHandleSize(_zhandle);
    if (ds == -1)
        done = 1;

    while (!done) {
        ftype = _rxhdr();

        if (ftype == 0xFF) {
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            if (_zdbg_got > 0 && _zdbg_hfail != 'D') {
                hdr_fails++;
                vdp_print((uint8_t *)"Header error (retrying)");
                _znl();
                _txzhex(_ZNAK, 0, 0, 0, 0);
                vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                if (hdr_fails < 10u)
                    continue;
            } else {
                vdp_print((uint8_t *)"Header error");
                _znl();
            }
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            break;
        }

        hdr_fails = 0u;

        switch (ftype) {

        case _ZRQINIT:
            /* Sender retrying init -- resend capabilities */
            _txzhex(_ZRINIT, 0, 4u, 0, _CANFDX | _CANOVIO);
            break;

        case _ZSINIT:
            /* Sender's optional init (attention string) -- drain, ACK */
            _drain_subpkt();
            _txzhex(_ZACK, 0, 0, 0, 0);
            break;

        case _ZFILE:
            {
                uint8_t rz = _rxzfile();
                if (rz != 0u) {
                    vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
                    vdp_print((uint8_t *)"File header error");
                    _znl();
                    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                    _txzhex(_ZNAK, 0, 0, 0, 0);
                    break;
                }
                vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                vdp_print((uint8_t *)"Receiving: ");
                vdp_print(_zfname);
                if (_zfsize > 0u) {
                    vdp_print((uint8_t *)" (");
                    if (_zfsize >= 1000u) {
                        _zprintdec32(_zfsize >> 10);
                        vdp_print((uint8_t *)" KB)");
                    } else {
                        _zprintdec32(_zfsize);
                        vdp_print((uint8_t *)" bytes)");
                    }
                }
                _znl();
            }
            if (fh != 0xFF && _zfpos == 0u && _zwrlen == 0u) {
                /* Sender retried the initial ZFILE before any data arrived.
                 * Keep the existing empty file handle and simply resend ZRPOS
                 * instead of close/reopen/truncate churn. */
                vdp_setTextColor(VDP_GRAY, VDP_BLACK);
                vdp_print((uint8_t *)"  (sender retry)");
                _znl();
                vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                _txpos(_ZRPOS, 0u);
                break;
            }
            /* Close any previously open file before starting a new transfer */
            if (fh != 0xFF) {
                _wrflush(fh);
                rn_fileHandleClose(fh);
                fh = 0xFF;
            }
            _zfpos    = 0u;
            _zwrlen   = 0;
            _zdat_row = 0xFF;  /* new file -- progress line starts on a fresh row */
            for (fname_len = 0;
                 _zfname[fname_len] && fname_len < (uint8_t)ZM_FNAME_MAX;
                 fname_len++)
                ;
            /* Open / create file then truncate to 0 bytes */
            /* Use handle 2 -- TCP is on handle 1, must not share */
            fh = rn_fileOpen(fname_len, _zfname, OPEN_FILE_FLAG_READWRITE, 2u);
            if (fh == 0xFF) {
                _txzhex(_ZSKIP, 0, 0, 0, 0);
                break;
            }
            rn_fileHandleEmptyFile(fh);
            /* Request data from position 0.
             * _txpos() appends XON (0x11) after the ZHEX frame --
             * required to uncork AmiExpress after its ZFILE ZCRCW. */
            _txpos(_ZRPOS, _zfpos);
            break;

        case _ZDATA:
            /* Incoming file data sub-packets */
            if (fh == 0xFF) {
                _txzhex(_ZNAK, 0, 0, 0, 0);
                break;
            }
            ttype = '?';
            {
                uint32_t pkt_start;
                for (;;) {
                    pkt_start = _zfpos;
                    term = _rxdata();
                    if (term == 0xFEu) {
                        /* Sub-packet too large: _rxdata() drained it.
                         * Restore _zfpos and send ZRPOS -- AmiExpress
                         * will halve its block_size and retransmit. */
                        _zfpos  = pkt_start;
                        _zwrlen = 0;
                        _txpos(_ZRPOS, _zfpos);
                        ttype = 'O';
                        break;
                    }
                    if (term == 0xFF) {
                        /* Timeout, CRC error, or disconnect */
                        ttype = _zdbg_rxfail;
                        _zwrlen = 0;
                        rn_fileHandleClose(fh);
                        fh = 0xFF;
                        _txzhex(_ZNAK, 0, 0, 0, 0);
                        break;
                    }
                    if (term == _ZCRCQ) {
                        ttype = 'q';
                        /* Release the sender first, then commit the buffered
                         * bytes to IA storage.  AmiExpress appears sensitive
                         * to checkpoint ACK latency. */
                        _txpos(_ZACK, _zfpos);
                        _wrflush(fh);
                        continue;
                    }
                    if (term == _ZCRCE || term == _ZCRCW) {
                        ttype = (term == _ZCRCE) ? 'e' : 'w';
                        if (term == _ZCRCW)
                            _txpos(_ZACK, _zfpos);
                        _wrflush(fh);
                        break;
                    }
                    /* _ZCRCG: no ACK required, so just commit the buffered
                     * bytes before continuing with the same frame. */
                    _wrflush(fh);
                    ttype = 'g';  /* _ZCRCG: data continues, no ACK */
                }
            }
            /* Update the in-place progress line AFTER the frame completes */
            if (_zdat_row == 0xFF) {
                _zdat_row = vdp_cursor.y;
            } else {
                vdp_setCursor2(0, _zdat_row);
            }
            vdp_setTextColor(VDP_GRAY, VDP_BLACK);
            vdp_print((uint8_t *)"  Received: ");
            _zprintdec32(_zfpos);
            vdp_print((uint8_t *)" bytes");
            if (ttype == 'O') vdp_print((uint8_t *)" (retry)    ");
            else               vdp_print((uint8_t *)"            ");
            _znl();
            if (ttype == 'C') {
                vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
                vdp_print((uint8_t *)"  Data CRC error - retrying");
                _znl();
            }
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            break;

        case _ZEOF:
            /* File transfer complete -- verify sender's file size matches ours */
            eof_pos = (uint32_t)_zhdr[0]
                    | ((uint32_t)_zhdr[1] << 8)
                    | ((uint32_t)_zhdr[2] << 16)
                    | ((uint32_t)_zhdr[3] << 24);
            if (eof_pos == _zfpos) {
                vdp_setTextColor(VDP_MED_GREEN, VDP_BLACK);
                vdp_print((uint8_t *)"Complete: ");
                _zprintdec32(_zfpos);
                vdp_print((uint8_t *)" bytes");
            } else {
                vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
                vdp_print((uint8_t *)"Size mismatch: got ");
                _zprintdec32(_zfpos);
                vdp_print((uint8_t *)" expected ");
                _zprintdec32(eof_pos);
            }
            _znl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            if (eof_pos != _zfpos) {
                _txpos(_ZRPOS, _zfpos);
                break;
            }
            if (fh != 0xFF) {
                _wrflush(fh);
                rn_fileHandleClose(fh);
                fh = 0xFF;
            }
            /* Ready for another file */
            _txzhex(_ZRINIT, 0, 4u, 0, _CANFDX | _CANOVIO);
            break;

        case _ZFIN:
            /* Session finished -- acknowledge and stop */
            _txzhex(_ZFIN, 0, 0, 0, 0);
            done = 1;
            break;

        case _ZNAK:
        case _ZABORT:
        case _ZFERR:
        default:
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            vdp_print((uint8_t *)"Transfer aborted");
            _znl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            if (fh != 0xFF) {
                rn_fileHandleClose(fh);
                fh = 0xFF;
            }
            done = 1;
            break;
        }
    }

    if (fh != 0xFF) {
        _wrflush(fh);
        rn_fileHandleClose(fh);
    }

    vdp_setTextColor(VDP_DARK_YELLOW, VDP_BLACK);
    vdp_print((uint8_t *)"ZModem done. Press any key...");
    _znl();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
    while (!isKeyPressed())
        ;
    getChar();
}
