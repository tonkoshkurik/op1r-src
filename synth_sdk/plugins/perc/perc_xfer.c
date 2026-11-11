#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc: PO-32 transfer reader. Record a PO-32 / Microtonic transfer into the slot's sample (input on a perc slot, or
 * load one with the sample browser); the main loop decodes it and its patches become the po-32 types (Shift + blue,
 * after the built-in sounds). One recording holds 6 s: the preamble and about 16 patch packets (8 instruments with
 * both morph sides), so start recording before the transfer starts.
 *
 * The DPSK demodulator and the framing (whitening, CRC state, packet layout, final tail) follow libpo32
 * (MIT, Copyright (c) 2026 Eric Lewis; po32.c, po_wire_internal.h); this is an integer port.
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "perc.h"
#include "perc_xfer.h"

/* ---- framing (libpo32's wire format) ---- */
static inline uint16_t crc_mix(uint16_t s, uint8_t x)
{
    uint16_t m = x;
    return (uint16_t)((((uint32_t)s << 8) | m) ^ ((uint32_t)m << 12) ^ ((uint32_t)m << 5));
}
static inline uint16_t crc_update(uint16_t s, uint8_t raw)
{
    uint8_t x = (uint8_t)(raw ^ (s >> 8));
    x ^= (uint8_t)(x >> 4);
    return crc_mix(s, x);
}
static inline uint8_t unwhiten(uint16_t s, uint8_t c) { return (uint8_t)(c - (s & 0xffu)); }

static int payload_ok(uint16_t tag, unsigned len)
{
    switch (tag) {
    case 0x37B2: return len == 43;                          /* patch */
    case 0xD022: return len == 211;                         /* pattern */
    case 0x505A: return len >= 35;                          /* state */
    case 0xA354: return len == 2;                           /* knob */
    case 0x4AB4: case 0xB892: case 0x496F: return len == 1; /* reset, erase, pattern slot */
    }
    return 0;
}

#define SYNC_LO 0x55555555u                                 /* the preamble's last 8 bytes, LSB first */
#define SYNC_HI 0xCF9D1914u

static void desync(perc_xfer_t *x) { x->synced = 0; x->work_len = 0; }

static void on_byte(perc_xfer_t *x, uint8_t b)
{
    if (x->work_len >= sizeof(x->work)) { desync(x); return; }
    x->work[x->work_len++] = b;
    if (unwhiten(x->crc, x->work[0]) == 0xC3) {             /* final tail: the transfer is complete */
        if (x->work_len >= 5) { x->done = 1; desync(x); }
        return;
    }
    if (x->work_len < 3) return;
    uint16_t s = x->crc;
    uint8_t lo = unwhiten(s, x->work[0]); s = crc_update(s, lo);
    uint8_t hi = unwhiten(s, x->work[1]); s = crc_update(s, hi);
    uint8_t n = unwhiten(s, x->work[2]);
    uint16_t tag = (uint16_t)(lo | (hi << 8));
    if (!payload_ok(tag, n)) { desync(x); return; }
    unsigned total = 5u + n;
    if (x->work_len < total) return;
    /* the whole packet: header, payload and the state trailer */
    s = x->crc;
    uint8_t raw[260];
    for (unsigned i = 0; i < 3u + n; i++) { raw[i] = unwhiten(s, x->work[i]); s = crc_update(s, raw[i]); }
    uint16_t trailer = s;
    uint8_t t0 = unwhiten(s, x->work[3 + n]); s = crc_update(s, t0);
    uint8_t t1 = unwhiten(s, x->work[4 + n]); s = crc_update(s, t1);
    if (t0 != (uint8_t)trailer || t1 != (uint8_t)(trailer >> 8)) { desync(x); return; }
    x->crc = s; x->work_len = 0; x->packets++;
    if (tag == 0x37B2) {                                    /* patch: command 0x1N left / 0x2N right, 21 x u16 LE */
        const uint8_t *p = raw + 3;
        unsigned side = (unsigned)(p[0] >> 4), inst = p[0] & 15u;
        if (side == 1 || side == 2) {
            uint16_t *dst = x->patch[inst][side - 1];
            for (int i = 0; i < 21; i++) dst[i] = (uint16_t)(p[1 + 2 * i] | (p[2 + 2 * i] << 8));
            x->have[inst] |= (uint8_t)side;
        }
    }
}

/* ---- DPSK demodulator: 1201.92 baud, carrier 3 cycles per symbol, 44.1 kHz ---- */
#define SYM_INC  117057150u                                  /* 1201.923 / 44100 x 2^32 */
#define CAR_INC  351171450u                                  /* 3 x SYM_INC */

void perc_xfer_begin(perc_xfer_t *x)
{
    unsigned char *p = (unsigned char *)x;
    for (unsigned i = 0; i < sizeof(*x); i++) p[i] = 0;
    x->sym = ((int64_t)1 << 32) - SYM_INC;                  /* the first sample ends a symbol, as libpo32 */
}

static void boundary(perc_xfer_t *x)
{
    if (x->started) {
        int64_t dot = (int64_t)x->ai * x->pi + (int64_t)x->aq * x->pq;
        uint32_t bit = dot >= 0;
        if ((x->pi > 0) != (x->ai > 0)) {                    /* timing recovery on an I sign change, gain 0.01 */
            int64_t a = x->ai < 0 ? -(int64_t)x->ai : x->ai, b = x->pi < 0 ? -(int64_t)x->pi : x->pi;
            if (a + b > 0) x->sym += ((a - b) * ((int64_t)1 << 32) / (a + b)) / 100;
        }
        if (!x->synced) {
            x->win_lo = (x->win_lo >> 1) | (x->win_hi << 31);
            x->win_hi = (x->win_hi >> 1) | (bit << 31);
            if (x->win_lo == SYNC_LO && x->win_hi == SYNC_HI && !x->done) {
                x->synced = 1; x->crc = 0x1D0F; x->cur = 0; x->bits = 0; x->work_len = 0;
            }
        } else {
            x->cur |= (uint8_t)(bit << x->bits);
            if (++x->bits == 8) { uint8_t b = x->cur; x->cur = 0; x->bits = 0; on_byte(x, b); }
        }
    } else x->started = 1;
    x->pi = x->ai; x->pq = x->aq; x->ai = 0; x->aq = 0;
}

void perc_xfer_push(perc_xfer_t *x, const int16_t *s, unsigned n)
{
    for (unsigned i = 0; i < n && !x->done; i++) {
        x->car += CAR_INC;
        int32_t sn = OP1_SINE_Q[(x->car >> 19) & 8191], cs = OP1_SINE_Q[((x->car >> 19) + 2048) & 8191];
        x->ai += (s[i] * sn) >> 8;
        x->aq += (s[i] * cs) >> 8;
        x->sym += SYM_INC;
        if (x->sym >= ((int64_t)1 << 32)) { x->sym -= (int64_t)1 << 32; boundary(x); }
    }
}

/* ---- per slot: the decoded patches, refreshed when the slot's sample changes ---- */
static perc_xfer_t dec;                                       /* the decode in progress */
static uint16_t bank[8][PERC_XFER_SLOTS][21];
static uint8_t bank_have[8][PERC_XFER_SLOTS];
static int dec_slot = -1;

const uint16_t *perc_xfer_patch(int instrument)
{
    if (instrument < 0 || instrument >= PERC_XFER_SLOTS) return 0;
    int slot = op1_current_slot() & 7;
    return bank_have[slot][instrument] ? bank[slot][instrument] : 0;
}
int perc_xfer_count(int slot)
{
    int n = 0;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) n += bank_have[slot & 7][i] != 0;
    return n;
}
int perc_xfer_busy(void) { return dec_slot >= 0; }

/* take a finished decode: its left sides become the slot's po-32 sounds (no patches: the bank stays) */
static void take(int slot, const perc_xfer_t *x)
{
    int any = 0;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) any |= x->have[i];
    if (!any) return;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) {
        bank_have[slot][i] = (x->have[i] & 1) != 0;
        if (bank_have[slot][i]) for (int j = 0; j < 21; j++) bank[slot][i][j] = x->patch[i][0][j];
    }
}

#ifdef OP1_HOST
/* host tests: decode a whole recording into a slot */
void perc_xfer_decode_into(int slot, const int16_t *s, unsigned n)
{
    perc_xfer_begin(&dec); perc_xfer_push(&dec, s, n); take(slot & 7, &dec);
}
const perc_xfer_t *perc_xfer_last(void) { return &dec; }
void perc_xfer_service(void) {}
#else
static uint32_t sig_of[8];                                    /* the sample's signature when decoded */
static uint32_t dec_pos, dec_len;
static const int16_t *dec_data;
static uint32_t signature(const uint8_t *sb)
{
    const int16_t *pk = op1_sample_peaks(sb);
    uint32_t h = 2166136261u;
    for (int i = 0; i < OP1_PEAK_BUCKETS; i++) { h ^= (uint16_t)pk[i]; h *= 16777619u; }
    return h ^ (uint32_t)(uintptr_t)op1_sample_data(sb);
}
/* main loop: bounded work per call (8192 samples) */
void perc_xfer_service(void)
{
    int slot = op1_current_slot() & 7;
    const uint8_t *sb = op1_sample_buffer(slot);
    if (!sb) return;
    if (dec_slot < 0) {
        uint32_t sig = signature(sb);
        if (sig == sig_of[slot]) return;
        sig_of[slot] = sig;
        perc_xfer_begin(&dec);
        dec_slot = slot; dec_pos = 0;
        dec_data = op1_sample_data(sb);
        int32_t cap = *(const int32_t *)(sb + 0x1038);
        dec_len = cap > 0 && cap < OP1_SAMPLE_MAX ? (uint32_t)cap : OP1_SAMPLE_MAX;
        return;
    }
    if (dec_slot != slot) { sig_of[dec_slot] = 0; dec_slot = -1; return; }   /* slot changed: decode again later */
    uint32_t n = dec_len - dec_pos; if (n > 8192) n = 8192;
    perc_xfer_push(&dec, dec_data + dec_pos, n);
    dec_pos += n;
    if (dec.done || dec_pos >= dec_len) { take(slot, &dec); dec_slot = -1; }
}
#endif
