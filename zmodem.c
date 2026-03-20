/*
 * zmodem.c -- ZModem receive implementation
 *
 * Included into nterm.c via  #include "zmodem.c"
 * Do NOT include NABU-LIB.h or RetroNET-FileStore.h here.
 *
 * Implements receive-only ZModem over the IA TCP connection.
 * Sends ZHEX frames; accepts ZHEX and ZBIN from the sender.
 * Does NOT advertise CANFC32 in ZRINIT -- forces CRC-16 only.
 * Received files are written to the IA file store (append-only).
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

/* ZRINIT capability flags we advertise */
#define _CANFDX   0x01
#define _CANOVIO  0x02
/* NOTE: CANFC32 (0x20) deliberately omitted -- forces CRC-16 */

/* Timeout: rn_TCPHandleRead poll iterations before giving up.
 * Each poll is one HCCA round-trip (~540μs real time at 111Kbps).
 * 200000 × 540μs ≈ 108 seconds.  If this is hit _zdbg_got stays 0. */
#define _ZTIMEOUT  200000u

/* -----------------------------------------------------------------------
 * Module state -- all static
 * --------------------------------------------------------------------- */
static uint8_t  _zhandle;           /* active TCP handle              */

static uint8_t  _zrx[128];          /* TCP receive buffer             */
static uint8_t  _zrxpos;            /* read index into _zrx           */
static uint8_t  _zrxlen;            /* valid bytes in _zrx            */

static uint8_t  _ztx[24];           /* TX staging (ZHEX frame = 20 B) */
static uint8_t  _ztxlen;

static uint8_t  _zwr[128];          /* file write buffer              */
static uint8_t  _zwrlen;

static uint8_t  _zhdr[4];           /* last received header data[0-3] */

static uint8_t  _zfname[ZM_FNAME_MAX + 1];
static uint32_t _zfpos;             /* bytes written to current file  */
static uint8_t  _zcrc32;            /* 1 = sender using CRC-32 frames */

static uint16_t _zcrc[256];         /* CRC-16 CCITT lookup table      */

/* -----------------------------------------------------------------------
 * CRC-16 CCITT (polynomial 0x1021, initial value 0x0000)
 * Table computed once at the start of each transfer session.
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

/* -----------------------------------------------------------------------
 * Low-level TCP I/O
 * --------------------------------------------------------------------- */

/* Diagnostics updated by _rxb() and _rxhdr() */
static int32_t  _zdbg_got;    /* last return from rn_TCPHandleRead */
static uint16_t _zdbg_polls;  /* poll iterations in last _rxb() refill */
static uint8_t  _zdbg_htype;  /* header type byte read after ZDLE      */
static uint8_t  _zdbg_hfail;  /* _rxhdr failure code:
                                *  'D'=drain disconnect, 'T'=htype read,
                                *  'U'=htype unknown,    'X'=unhex fail,
                                *  'C'=CRC mismatch,     'B'=ZBIN parse */
static uint16_t _zdbg_crc_c;  /* CRC we computed  (on CRC mismatch)    */
static uint16_t _zdbg_crc_r;  /* CRC we received  (on CRC mismatch)    */

/* Read one raw byte from TCP stream.
 * Refills _zrx from TCP when the buffer is empty.
 * Returns byte (0-255 as int16_t), or -1 on timeout / disconnect.
 *
 * Uses rn_TCPHandleRead directly (not SIZE then READ) so each poll
 * is a single HCCA round-trip.  Returns -1 for both disconnect and
 * timeout; callers distinguish via _zdbg_got:
 *   -1  = TCP disconnected
 *    0  = timeout (no data arrived in _ZTIMEOUT polls)
 */
static int16_t _rxb(void)
{
    uint32_t retry;
    int32_t  got;

    if (_zrxpos < _zrxlen)
        return (int16_t)_zrx[_zrxpos++];

    _zdbg_polls = 0u;
    for (retry = 0u; retry < _ZTIMEOUT; retry++) {
        got = rn_TCPHandleRead(_zhandle, _zrx, 0, 128u);
        _zdbg_got = got;
        if (got < 0) return -1;    /* TCP disconnected (-1) */
        if (got > 0) {
            _zrxlen = (uint8_t)got;
            _zrxpos = 0;
            return (int16_t)_zrx[_zrxpos++];
        }
        if (_zdbg_polls < 0xFFFFu) _zdbg_polls++;
    }
    return -1;  /* timeout -- _zdbg_got stays 0 */
}

static int32_t _zlast_wr;   /* return value of last rn_TCPHandleWrite */

static void _txflush(void)
{
    if (_ztxlen > 0u) {
        _zlast_wr = rn_TCPHandleWrite(_zhandle, 0, _ztxlen, _ztx);
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
 * Display helpers (scrolling debug output)
 * --------------------------------------------------------------------- */
static void _znl(void)
{
    vdp_newLine();
    vdp_setCursor2(0, vdp_cursor.y);
}

/* Print uint8_t as 2 hex chars */
static void _zphex(uint8_t v)
{
    vdp_write(_nib(v >> 4));
    vdp_write(_nib(v));
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
 * --------------------------------------------------------------------- */
static int16_t _rxzd(void)
{
    int16_t b;
    b = _rxb();
    if (b < 0) return -1;
    if ((uint8_t)b != _ZD) return b;
    b = _rxb();
    if (b < 0) return -1;
    switch ((uint8_t)b) {
    case _ZCRCE: return (int16_t)0x100;
    case _ZCRCG: return (int16_t)0x101;
    case _ZCRCQ: return (int16_t)0x102;
    case _ZCRCW: return (int16_t)0x103;
    default:     return (int16_t)((uint8_t)b ^ 0x40u);
    }
}

/* -----------------------------------------------------------------------
 * _txzhex -- transmit one ZHEX header frame
 *
 * Wire format: ** ZDLE 'B' TT D0D0 D1D1 D2D2 D3D3 CCCC CR LF
 * (TT and each Dx are 2 hex chars; CCCC = 4 hex chars CRC-16)
 * CRC covers: type, d0, d1, d2, d3 (5 bytes, init=0).
 * --------------------------------------------------------------------- */
static void _txzhex(uint8_t type,
                    uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3)
{
    uint16_t crc;
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
    _txflush();
}

/* -----------------------------------------------------------------------
 * _rxhdr -- receive a ZHEX or ZBIN header frame
 *
 * Drains incoming bytes until ZDLE, then reads the frame type byte,
 * 4 data bytes, and 2-byte CRC.  Verifies CRC.
 * Stores data bytes in _zhdr[0..3] (ZP0..ZP3).
 * Returns frame type (0-12), or 0xFF on error / timeout.
 * --------------------------------------------------------------------- */
static uint8_t _rxhdr(void)
{
    int16_t  b;
    uint8_t  htype;
    uint8_t  d[5];       /* d[0]=type, d[1-4]=ZP0-ZP3 */
    uint16_t crc, rcrc;
    uint8_t  i;
    int16_t  hi, lo;
    uint8_t  skip_crc;   /* 1 = CRC-32 fallback applied, skip common check */

    /* Drain until ZDLE -- skips ZPADs, XON, and any inter-frame junk */
    _zdbg_htype = 0;
    _zdbg_hfail = '?';
    skip_crc    = 0;
    rcrc        = 0u;
    for (;;) {
        b = _rxb();
        if (b < 0) { _zdbg_hfail = 'D'; return 0xFF; }
        if ((uint8_t)b == _ZD) break;
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
         * Some senders use CRC-32 (4 bytes) with the ZBIN 'A' marker;
         * if CRC-16 fails but the type byte is valid, consume the extra
         * 2 CRC bytes and accept with CRC-32 fallback. */
        for (i = 0u; i < 5u; i++) {
            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            d[i] = (uint8_t)b;
        }
        b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
        rcrc = (uint16_t)(((uint16_t)(uint8_t)b) << 8);
        b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
        rcrc |= (uint8_t)b;
        /* Pre-check CRC-16; if it fails and type is valid, assume CRC-32 */
        crc = 0u;
        for (i = 0u; i < 5u; i++) crc = _crc16(crc, d[i]);
        if (crc != rcrc) {
            _zdbg_crc_c = crc;
            _zdbg_crc_r = rcrc;
            if (d[0] <= (uint8_t)_ZFERR) {
                _rxzd(); _rxzd();  /* discard remaining 2 CRC-32 bytes */
                _zdbg_hfail = 'K';
                _zcrc32     = 1;
                skip_crc    = 1;
            } else {
                _zdbg_hfail = 'C';
                return 0xFF;
            }
        }

    } else if (htype == 0x43u) {
        /* ZBIN32 ('C'): 5 ZDLE-decoded bytes, then 4 ZDLE-decoded CRC-32 bytes.
         * CRC-32 not verified (no table on Z80); accept if type is valid. */
        for (i = 0u; i < 5u; i++) {
            b = _rxzd();
            if (b < 0 || b > (int16_t)0xFF) { _zdbg_hfail = 'B'; return 0xFF; }
            d[i] = (uint8_t)b;
        }
        _rxzd(); _rxzd(); _rxzd(); _rxzd();  /* consume 4 CRC-32 bytes */
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
    _rxzd(); _rxzd();                      /* discard CRC bytes 1 & 2 */
    if (_zcrc32) { _rxzd(); _rxzd(); }    /* discard CRC bytes 3 & 4 */
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
 * _rxzfile -- read ZFILE data sub-packet; extract filename into _zfname
 * The ZFILE sub-packet begins with a null-terminated filename followed
 * by a space, decimal file size, and other optional fields.
 * Returns 0 on success, 1 on CRC error or timeout.
 * --------------------------------------------------------------------- */
static uint8_t _rxzfile(void)
{
    int16_t  b;
    uint8_t  fi, fname_done;
    uint16_t crc, rcrc;
    uint8_t  term;

    fi         = 0u;
    fname_done = 0u;
    crc        = 0u;
    _zfname[0] = 0;

    for (;;) {
        b = _rxzd();
        if (b < 0) return 1;

        if (b > (int16_t)0xFF) {
            /* Sub-packet terminator */
            term = (uint8_t)((uint16_t)b - 0x100u + _ZCRCE);
            crc  = _crc16(crc, term);
            /* Read 2 ZDLE-decoded CRC bytes */
            b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) return 1;
            rcrc = (uint16_t)(((uint16_t)(uint8_t)b) << 8);
            b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) return 1;
            rcrc |= (uint8_t)b;
            if (crc != rcrc) {
                /* CRC-32 fallback: consume extra 2 bytes and accept */
                if (!_zcrc32 || _rxzd() < 0 || _rxzd() < 0) return 1;
            }
            break;
        }

        crc = _crc16(crc, (uint8_t)b);

        if (!fname_done) {
            if ((uint8_t)b == 0u) {
                /* null terminator of filename in stream */
                _zfname[fi] = 0;
                fname_done = 1u;
            } else if (fi < (uint8_t)ZM_FNAME_MAX) {
                _zfname[fi++] = (uint8_t)b;
            }
        }
    }

    if (!fname_done)
        _zfname[fi <= (uint8_t)ZM_FNAME_MAX ? fi : (uint8_t)ZM_FNAME_MAX] = 0;

    return 0;
}

/* -----------------------------------------------------------------------
 * _rxdata -- receive one data sub-packet and write bytes to file
 *
 * Returns the terminator byte (_ZCRCE/_ZCRCG/_ZCRCQ/_ZCRCW),
 * or 0xFF on timeout or CRC error.
 * --------------------------------------------------------------------- */
static uint8_t _rxdata(uint8_t fh)
{
    int16_t  b;
    uint8_t  dat, term;
    uint16_t crc, rcrc;

    crc = 0u;

    for (;;) {
        b = _rxzd();
        if (b < 0) return 0xFF;

        if (b > (int16_t)0xFF) {
            /* Terminator: include terminator byte itself in CRC */
            term = (uint8_t)((uint16_t)b - 0x100u + _ZCRCE);
            crc  = _crc16(crc, term);
            /* Read 2 ZDLE-decoded CRC bytes */
            b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) return 0xFF;
            rcrc = (uint16_t)(((uint16_t)(uint8_t)b) << 8);
            b = _rxzd(); if (b < 0 || b > (int16_t)0xFF) return 0xFF;
            rcrc |= (uint8_t)b;
            if (crc != rcrc) {
                /* CRC-32 fallback: consume extra 2 bytes and accept */
                if (!_zcrc32 || _rxzd() < 0 || _rxzd() < 0) return 0xFF;
            }
            _wrflush(fh);
            return term;
        }

        dat = (uint8_t)b;
        crc = _crc16(crc, dat);
        _zwr[_zwrlen++] = dat;
        _zfpos++;
        if (_zwrlen >= 128u)
            _wrflush(fh);
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
    int32_t  ds;

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

    _zdbg_got    = 0;
    _zdbg_polls  = 0u;
    _zdbg_htype  = 0;
    _zdbg_hfail  = '?';
    _zdbg_crc_c  = 0u;
    _zdbg_crc_r  = 0u;
    _zlast_wr    = 0;
    _zcrc32      = 0;
    _crc_init();

    vdp_clearScreen();
    vdp_setCursor2(0, 0);
    vdp_setTextColor(VDP_CYAN, VDP_BLACK);
    vdp_print((uint8_t *)"ZModem: start pre=");
    _zphex(prelen);
    _znl();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);

    /* Advertise capabilities in ZF0 (= ZP0 = d0); omit CANFC32 for CRC-16 */
    _txzhex(_ZRINIT, _CANFDX | _CANOVIO, 0, 0, 0);

    /* Check TCP state and write result after sending ZRINIT */
    ds = rn_TCPHandleSize(_zhandle);
    vdp_setTextColor(VDP_GRAY, VDP_BLACK);
    vdp_print((uint8_t *)"TCP:");
    vdp_write(ds == -1 ? 'D' : 'K');
    vdp_print((uint8_t *)" wr=");
    vdp_write(_zlast_wr >= 0 ? 'Y' : 'N');
    _znl();
    vdp_setTextColor(VDP_WHITE, VDP_BLACK);

    while (!done) {
        ftype = _rxhdr();

        vdp_setTextColor(VDP_GRAY, VDP_BLACK);
        vdp_print((uint8_t *)"hdr=");
        _zphex(ftype);
        _znl();
        vdp_setTextColor(VDP_WHITE, VDP_BLACK);

        if (ftype == 0xFF) {
            vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
            /* fail=X: D=drain disc, T=htype disc, U=unknown htype,
             *         X=unhex err, B=ZBIN parse, C=CRC mismatch    */
            vdp_print((uint8_t *)"fail=");
            vdp_write(_zdbg_hfail);
            vdp_print((uint8_t *)" ht=");
            _zphex(_zdbg_htype);
            _znl();
            if (_zdbg_hfail == 'C') {
                vdp_print((uint8_t *)"crc c=");
                _zphex((uint8_t)(_zdbg_crc_c >> 8));
                _zphex((uint8_t)_zdbg_crc_c);
                vdp_print((uint8_t *)" r=");
                _zphex((uint8_t)(_zdbg_crc_r >> 8));
                _zphex((uint8_t)_zdbg_crc_r);
                _znl();
            }
            vdp_print((uint8_t *)"got=");
            _zphex((uint8_t)(_zdbg_got >> 8));
            _zphex((uint8_t)_zdbg_got);
            vdp_print((uint8_t *)" p=");
            _zphex((uint8_t)(_zdbg_polls >> 8));
            _zphex((uint8_t)_zdbg_polls);
            _znl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            break;
        }

        switch (ftype) {

        case _ZRQINIT:
            /* Sender retrying init -- resend our capabilities */
            _txzhex(_ZRINIT, _CANFDX | _CANOVIO, 0, 0, 0);
            break;

        case _ZSINIT:
            /* Sender's optional init (attention string) -- drain, ACK */
            _drain_subpkt();
            _txzhex(_ZACK, 0, 0, 0, 0);
            break;

        case _ZFILE:
            /* Close any previously open file before starting a new transfer */
            if (fh != 0xFF) {
                _wrflush(fh);
                rn_fileHandleClose(fh);
                fh = 0xFF;
            }
            _zfpos  = 0u;
            _zwrlen = 0;
            {
                uint8_t rz = _rxzfile();
                vdp_setTextColor(VDP_GRAY, VDP_BLACK);
                vdp_print((uint8_t *)"zfile=");
                vdp_write(rz == 0u ? 'Y' : 'N');
                vdp_write(' ');
                vdp_print(_zfname);
                _znl();
                vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                if (rz != 0u) {
                    _txzhex(_ZNAK, 0, 0, 0, 0);
                    break;
                }
            }
            for (fname_len = 0;
                 _zfname[fname_len] && fname_len < (uint8_t)ZM_FNAME_MAX;
                 fname_len++)
                ;
            /* Open / create file then truncate to 0 bytes */
            fh = rn_fileOpen(fname_len, _zfname, OPEN_FILE_FLAG_READWRITE, 0xFF);
            vdp_setTextColor(VDP_GRAY, VDP_BLACK);
            vdp_print((uint8_t *)"open=");
            vdp_write(fh != 0xFF ? 'Y' : 'N');
            _znl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            if (fh == 0xFF) {
                _txzhex(_ZSKIP, 0, 0, 0, 0);
                break;
            }
            rn_fileHandleEmptyFile(fh);
            /* Request data from position 0 */
            _txzhex(_ZRPOS, 0, 0, 0, 0);
            vdp_setTextColor(VDP_GRAY, VDP_BLACK);
            vdp_print((uint8_t *)"rp=");
            vdp_write(_zlast_wr >= 0 ? 'Y' : 'N');
            _znl();
            vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            break;

        case _ZDATA:
            /* Incoming file data sub-packets */
            if (fh == 0xFF) {
                _txzhex(_ZNAK, 0, 0, 0, 0);
                break;
            }
            for (;;) {
                term = _rxdata(fh);
                if (term == 0xFF) {
                    /* Error (CRC mismatch or timeout) -- abort */
                    vdp_setTextColor(VDP_LIGHT_RED, VDP_BLACK);
                    vdp_print((uint8_t *)"dat! p=");
                    _zphex((uint8_t)(_zfpos >> 8));
                    _zphex((uint8_t)_zfpos);
                    _znl();
                    vdp_setTextColor(VDP_WHITE, VDP_BLACK);
                    rn_fileHandleClose(fh);
                    fh = 0xFF;
                    _txzhex(_ZNAK, 0, 0, 0, 0);
                    break;
                }
                if (term == _ZCRCQ) {
                    /* Sender requests ACK, but transfer continues */
                    _txzhex(_ZACK,
                            (uint8_t)(_zfpos),
                            (uint8_t)(_zfpos >> 8),
                            (uint8_t)(_zfpos >> 16),
                            (uint8_t)(_zfpos >> 24));
                }
                if (term == _ZCRCE || term == _ZCRCW) {
                    /* End of this ZDATA frame */
                    if (term == _ZCRCW) {
                        _txzhex(_ZACK,
                                (uint8_t)(_zfpos),
                                (uint8_t)(_zfpos >> 8),
                                (uint8_t)(_zfpos >> 16),
                                (uint8_t)(_zfpos >> 24));
                    }
                    break;  /* wait for ZEOF or next ZDATA */
                }
                /* _ZCRCG: data continues, no ACK needed */
            }
            break;

        case _ZEOF:
            /* File transfer complete */
            if (fh != 0xFF) {
                _wrflush(fh);
                rn_fileHandleClose(fh);
                fh = 0xFF;
                vdp_setTextColor(VDP_MED_GREEN, VDP_BLACK);
                vdp_print((uint8_t *)"transfer complete");
                _znl();
                vdp_setTextColor(VDP_WHITE, VDP_BLACK);
            }
            /* Ready for another file */
            _txzhex(_ZRINIT, _CANFDX | _CANOVIO, 0, 0, 0);
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
            vdp_print((uint8_t *)"abrt=");
            _zphex(ftype);
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
