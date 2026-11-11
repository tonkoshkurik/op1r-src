#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc drum kit (plugin.json "kind": "drum", docs/43): a drum type next to drum, dbox and drum+. Each of the 24 keys
 * plays its own perc sound (a PO-32 patch: a built-in one, or one transferred into the kit's sample), with its own
 * pitch, length, punch, tone, drive, level and morph, saved with the kit compactly:
 *   "op1r": {"engine": "perc", "pk": [[sound, pitch, length, punch, tone, drive, level, morph], x 24]}
 * A built-in sound rebuilds its 21+21 patch words from its index; a transferred PO-32 sound appends them (50
 * ints). Earlier kits ("perc": int arrays) are read too. A fresh kit is ~1 KB (a big patch froze R246 on save).
 * A perc kit is made from a sampled drum kit (Shift + T1, "perc"); its sample is not played: a PO-32 transfer
 * recorded into it (12 s: a whole kit, both sides) brings sounds.
 *
 * Voices: the 12 drum voices of the DrumSampler; the stock voice fields (+8: pos, state, blocks) are kept as the stock
 * note_on / note_off / render do, so the firmware's voice allocation works as for a sampled kit. At most PD_SYNTH voices
 * synthesise at once (the oldest fades out). GPL-3.0-or-later with the OP1R firmware permission. */
#include "../../op1_synth.h"
#include "../percsyn/perc_dsp.h"
#include "../percsyn/perc_tables.h"
#include "../percsyn/perc_xfer.h"
#include "../percsyn/perc_draw.h"

#define PD_SLOTS  8
#define PD_PADS   24
#define PD_VOICES 12
#define PD_SYNTH  6                                           /* voices synthesising at once (CPU) */
#define PD_NF     50                                          /* stored ints per key */
#define PD_NF_OLD 28                                          /* before morph */
#define PD_XFER   16

#ifndef OP1_HOST
#define VOICE_OBJ  ((void *const *)0x01AAAD30)                /* the 12 voice objects */
#define VOICE_PAD  ((const int32_t *)0xFF801334)              /* pad of voice v */
#define VOICE_VEL  ((const int16_t *)0xFF8012EC)              /* velocity << 8 of voice v */
#define DRUM_SLOT  (*(const volatile int32_t *)0x01AAAC10)    /* the playing drum slot */
#define SCREEN_TABLE    ((void **)0x01DF4E54)
#define CURRENT_SCREEN  (*(volatile int *)0x018B65C8)
#define DRUM_MODE       (*(volatile int *)0x01DE4C40 == 1)
#define DRUM_SLOT_CUR   (*(volatile int *)0xFF8016D8 & 7)
#define LAST_PAD        (*(volatile int *)0xFF801044)
#define SHIFT_HELD      (*(volatile uint8_t *)0x01DD7F78)
#define DRUM_TO_PATCH   ((void (*)(void *))0x019FFC50)
#define DRUM_PATCH(s)   ((void *)(0x01DDD9A8 + (s) * 0x38))
#define DRUM_SAMPLER(s) (((uint32_t **)0x01CC10CC)[s])        /* DrumSampler of slot s: [0] = SampleBuffer */
#define TYPE_JOB    ((void (*)(void *))0x0193F86C)
#define SAVE_SLOT   ((void (*)(void *))0x01940574)
#define UI_SLOT     ((void (*)(int))0x019C2BA0)
#define UI_SIGNAL   ((void (*)(void))0x019C2F04)
#define JV_KEY      ((void *(*)(void *, const char *))0x019BFEB4)
#define JV_IDX      ((void *(*)(void *, unsigned))0x019BFAF8)
#define JV_ASSIGN   ((void *(*)(void *, const void *))0x019BDDC0)
#define JV_SIZE     ((unsigned (*)(const void *))0x019BE8D8)
#define JV_CGET     ((const void *(*)(const void *, const char *))0x019BEB70)
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890)
#define JV_FROM_STR ((void *(*)(void *, const char *))0x019BDC4C)
#define JV_DTOR     ((void (*)(void *, int))0x019BDD18)
#define JV_REMOVE   ((void (*)(void *, void *, const char *))0x019BEE88)
void dpu_sync_auto(void);                                     /* core (dispatcher.c): pick the pages again */
#endif
enum { JV_NULL = 0, JV_INT = 1, JV_UINT = 2, JV_STRING = 4, JV_ARRAY = 6, JV_OBJECT = 7 };
typedef struct { int32_t pos, state; uint32_t blocks; } stock_voice;   /* +8 of a voice object */
enum { ST_OFF = 0, ST_HELD = 1, ST_RELEASED = 2 };

/* ---- kit state ---- */
typedef struct { uint16_t patch[21]; int16_t pitch, length, punch, tone, drive, level; uint8_t sound; int16_t morph; uint16_t right[21]; } pd_key;
static pd_key kit[PD_SLOTS][PD_PADS];
static volatile uint8_t claimed[PD_SLOTS];

/* a fresh kit: kick, snare, hats, clap, toms, ... with a few variations (sound, pitch / length / punch offsets) */
static const int8_t def_map[PD_PADS][4] = {
    { 0, 0, 0, 0 }, { 0, -5, 4, 4 }, { 1, 0, 0, 0 }, { 7, 0, 0, 0 }, { 3, 0, 0, 0 }, { 2, 0, 0, 0 },
    { 2, 0, 10, 0 }, { 5, -5, 0, 0 }, { 5, 0, 0, 0 }, { 5, 5, 0, 0 }, { 6, 0, 0, 0 }, { 8, 0, 0, 0 },
    { 9, 0, 0, 0 }, { 4, -5, 0, 0 }, { 4, 0, 0, 0 }, { 4, 7, 0, 0 }, { 0, 2, -4, 8 }, { 1, 2, -4, 0 },
    { 3, 0, 6, 0 }, { 2, 3, -6, 0 }, { 6, 0, -6, 0 }, { 8, 7, 0, 0 }, { 7, 5, 0, 0 }, { 9, 0, 6, 0 },
};
static void key_default(pd_key *k, int pad)
{
    const int8_t *d = def_map[pad];
    for (int i = 0; i < 21; i++) { k->patch[i] = PERC_FACTORY[d[0]][i]; k->right[i] = PERC_FACTORY_R[d[0]][i]; }
    k->morph = 0;
    k->sound = (uint8_t)d[0];
    k->pitch = (int16_t)(16384 + d[1] * 683);                 /* 683 per semitone (+-24 over the knob) */
    k->length = (int16_t)(16384 + d[2] * 1024);
    k->punch = (int16_t)(16384 + d[3] * 1024);
    k->tone = 16384; k->drive = 0; k->level = 16384;
}
static int16_t clamp15(int32_t v) { return (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v); }
static int key_sided(const pd_key *k) { for (int i = 0; i < 21; i++) if (k->patch[i] != k->right[i]) return 1; return 0; }
/* the patch a hit plays: left + (right - left) x morph, per word (libpo32's morph model; modes switch at their
 * thresholds). morph 0 plays the left side as is. */
static const uint16_t *key_patch(const pd_key *k, uint16_t *tmp)
{
    int32_t m = k->morph; if (m <= 0) return k->patch;
    m += m >> 14;                                             /* 32767 -> 32768: the right side exactly */
    for (int i = 0; i < 21; i++) tmp[i] = (uint16_t)(k->patch[i] + (((int32_t)k->right[i] - k->patch[i]) * m >> 15));
    return tmp;
}

/* knob words -> the voice's offsets (as the synth engine's macros) */
static void macros_of(const pd_key *k, perc_macro_t *m)
{
    m->pitch = (k->pitch - 16384) * 8;
    m->length = (k->length - 16384) * 13 + ((k->length - 16384) >> 2);
    m->punch = (k->punch - 16384) * 4;
    m->tone = (k->tone - 16384) * 12;
    m->dist = k->drive * 2;
    m->level_db = (k->level - 16384) * 48;
}

/* ---- voices (audio interrupt) ---- */
static perc_voice_t pv[PD_VOICES];
static volatile uint8_t pv_on[PD_VOICES], pv_fade[PD_VOICES], pv_pending[PD_VOICES];
static uint8_t pv_slot[PD_VOICES], pv_pad[PD_VOICES], pv_vel[PD_VOICES];
static uint32_t pv_t[PD_VOICES], pv_clock;
volatile uint32_t pd_hit_seq; volatile int32_t pd_hit_vel;

/* ---- breadcrumbs (debug build): the last step each thread entered, shown top-right on the perc page as
 * "u<ui> a<audio> t<tick>". A freeze leaves the last frame on screen: tick frozen + a moving stopped -> that thread
 * hung there; tick moving but animation lags -> heavy per-frame work at that step. Remove by clearing PERC_BC. */
#define PERC_BC 1
#if PERC_BC
volatile uint32_t perc_bc_ui, perc_bc_au, perc_bc_tick;
#define BCU(x) (perc_bc_ui = (x))
#define BCA(x) (perc_bc_au = (x))
#else
#define BCU(x) ((void)0)
#define BCA(x) ((void)0)
#endif

#ifndef OP1_HOST
static int voice_index(void *self) { for (int i = 0; i < PD_VOICES; i++) if (VOICE_OBJ[i] == self) return i; return -1; }
static stock_voice *model(void *self) { return (stock_voice *)((uint8_t *)self + 8); }

static void pd_note_on(void *self, const void *pad)
{
    (void)pad;
    int v = voice_index(self), slot = DRUM_SLOT & 7;
    stock_voice *m = model(self);
    m->state = ST_HELD; m->blocks = 0; m->pos = 0;
    if (v < 0) return;
    int p = VOICE_PAD[v]; if ((unsigned)p >= PD_PADS) p = 0;
    int vel = VOICE_VEL[v] >> 8;
    pv_slot[v] = (uint8_t)slot; pv_pad[v] = (uint8_t)p; pv_vel[v] = (uint8_t)vel;
    BCA(40);
    pv_fade[v] = 0; pv_t[v] = ++pv_clock;
    pv_pending[v] = 1;                                        /* set up in render: one context builds the voice */
    pv_on[v] = 1;                                             /* (after pending: render never sees on without it) */
    int n = 0, oldest = -1;                                   /* at most PD_SYNTH: the oldest other fades out */
    for (int i = 0; i < PD_VOICES; i++) if (pv_on[i] && !pv_fade[i]) { n++; if (i != v && (oldest < 0 || pv_t[i] < pv_t[oldest])) oldest = i; }
    if (n > PD_SYNTH && oldest >= 0) pv_fade[oldest] = 1;
    pd_hit_vel = vel * 258; pd_hit_seq++;
}
static void pd_note_off(void *self, const void *pad) { (void)pad; stock_voice *m = model(self); if (m->state == ST_HELD) m->state = ST_RELEASED; }

static void pd_render(void *self, int16_t *out, const void *pad)
{
    (void)pad;
    int v = voice_index(self), j;
    stock_voice *m = model(self);
    m->blocks++;
    if (v < 0 || !pv_on[v]) { for (j = 0; j < 128; j++) out[j] = 0; m->state = ST_OFF; return; }
    if (pv_pending[v]) {
        pv_pending[v] = 0;
        const pd_key *k = &kit[pv_slot[v]][pv_pad[v]];
        perc_macro_t mc; macros_of(k, &mc);
        static uint16_t mp[21];                               /* audio context only */
        BCA(41); perc_prepare(&pv[v].p, key_patch(k, mp), &mc, pv_vel[v]);
        perc_trigger(&pv[v]);
        pv[v].prev = 0;
    }
    BCA(42);
    int16_t half[64];
    int alive = perc_render(&pv[v], half, 64, 0);              /* pitch: the key's own (the stock pitch word is not used) */
    int32_t prev = pv[v].prev;
    for (j = 0; j < 64; j++) { int32_t c = half[j]; out[2 * j] = (int16_t)((prev + c) >> 1); out[2 * j + 1] = (int16_t)c; prev = c; }
    pv[v].prev = prev;
    if (pv_fade[v]) {                                          /* stolen: one block down to silence */
        for (j = 0; j < 128; j++) out[j] = (int16_t)(out[j] * (127 - j) >> 7);
        alive = 0;
    }
    if (!alive) { pv_on[v] = 0; pv_fade[v] = 0; m->state = ST_OFF; }
}
#endif

/* ---- the kit's transfer: PO-32 patches decoded from the slot's sample (GUI thread, bounded per frame) ---- */
static uint16_t xf_patch[PD_SLOTS][PD_XFER][2][21];   /* [left, right]; right = left when not sent */
static uint8_t xf_have[PD_SLOTS][PD_XFER];
static perc_xfer_t xf_dec;
static int xf_slot = -1;
static uint32_t xf_pos, xf_len, xf_sig[PD_SLOTS];
static const int16_t *xf_data;
#ifndef OP1_HOST
static const uint8_t *kit_sample(int slot) { uint32_t *o = DRUM_SAMPLER(slot & 7); return o && o[0] ? (const uint8_t *)o[0] : 0; }
static uint32_t kit_length(const uint8_t *sb)             /* the recording's length from its peaks (as granular) */
{
    const int16_t *pk = (const int16_t *)(sb + 4);
    for (int i = OP1_PEAK_BUCKETS - 1; i >= 0; i--) if (pk[i] > 96) return (uint32_t)(i + 1) * 256u;
    return 0;
}
static void xfer_step(int slot)
{
    const uint8_t *sb = kit_sample(slot);
    if (!sb) return;
    if (xf_slot < 0) {
        BCU(20);
        const int16_t *pk = (const int16_t *)(sb + 4);
        uint32_t h = 2166136261u;
        for (int i = 0; i < OP1_PEAK_BUCKETS; i++) { h ^= (uint16_t)pk[i]; h *= 16777619u; }
        h ^= (uint32_t)(uintptr_t)sb;
        if (h == xf_sig[slot]) return;
        xf_sig[slot] = h;
        xf_len = kit_length(sb);
        if (!xf_len) return;
        BCU(21); perc_xfer_begin(&xf_dec); xf_slot = slot; xf_pos = 0;
        xf_data = *(const int16_t *const *)sb + 8;           /* after the 8 guard samples */
        return;
    }
    if (xf_slot != slot) { xf_sig[xf_slot] = 0; xf_slot = -1; return; }
    uint32_t n = xf_len - xf_pos; if (n > 2048) n = 2048;   /* smaller chunk: the page stays smooth while decoding */
    BCU(22); perc_xfer_push(&xf_dec, xf_data + xf_pos, n); xf_pos += n; BCU(23);
    if (xf_dec.done || xf_pos >= xf_len) {
        int any = 0; for (int i = 0; i < PD_XFER; i++) any |= xf_dec.have[i] & 1;
        if (any) for (int i = 0; i < PD_XFER; i++) {
            xf_have[slot][i] = xf_dec.have[i] & 1;
            if (xf_have[slot][i]) for (int j = 0; j < 21; j++) {
                xf_patch[slot][i][0][j] = xf_dec.patch[i][0][j];
                xf_patch[slot][i][1][j] = xf_dec.have[i] & 2 ? xf_dec.patch[i][1][j] : xf_dec.patch[i][0][j];
            }
        }
        xf_slot = -1;
    }
}
#endif
/* the sound list: 0..9 built-in, 10.. po-32 n of this kit's transfer (0 if that one is absent); side 0 left, 1 right */
static const uint16_t *sound_patch(int slot, int s, int side)
{
    if (s < PERC_FACTORY_COUNT) return side ? PERC_FACTORY_R[s] : PERC_FACTORY[s];
    s -= PERC_FACTORY_COUNT;
    return s < PD_XFER && xf_have[slot][s] ? xf_patch[slot][s][side] : 0;
}

/* ---- persistence ---- */
#ifndef OP1_HOST
static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
static int32_t jv_int(const void *v) { int32_t w = *(const int32_t *)v; return jv_type(v) == JV_INT || (jv_type(v) == JV_UINT && w >= 0) ? w : 0; }
static int jv_is_str(const void *v, const char *s)
{
    if (!v || jv_type(v) != JV_STRING) return 0;
    const char *p = *(const char *const *)v;
    if (!p) return 0;
    while (*s && *p == *s) p++, s++;
    return *p == *s;
}
static int ours(const void *op1r) { return op1r && jv_type(op1r) == JV_OBJECT && jv_is_str(JV_CGET(op1r, "engine"), "perc"); }
static void key_get(pd_key *k, int f, int32_t x)
{
    uint16_t w = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x);
    if (f < 21) k->patch[f] = w;
    else if (f < 27) (&k->pitch)[f - 21] = clamp15(x);
    else if (f == 27) k->sound = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    else if (f == 28) k->morph = clamp15(x);
    else k->right[f - 29] = w;
}
static int32_t key_put(const pd_key *k, int f)
{
    return f < 21 ? k->patch[f] : f < 27 ? (&k->pitch)[f - 21] : f == 27 ? k->sound : f == 28 ? k->morph : k->right[f - 29];
}

/* a built-in key saved before the built-ins had a right side: give it the right side */
static void key_fix_right(pd_key *k)
{
    if (k->sound >= PERC_FACTORY_COUNT) return;
    for (int i = 0; i < 21; i++) if (k->patch[i] != PERC_FACTORY[k->sound][i] || k->right[i] != k->patch[i]) return;
    for (int i = 0; i < 21; i++) k->right[i] = PERC_FACTORY_R[k->sound][i];
}
static uint16_t clampw(int32_t x) { return (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); }
/* compact per-key save ("pk", docs/43): sound, 6 knobs, morph (8 ints); a built-in sound rebuilds its patch from
 * PERC_FACTORY/PERC_FACTORY_R by index, so only a transferred sound also stores its 21+21 patch words (50 ints).
 * A fresh kit saves ~1 KB, well under the stock save path's budget (a 5 KB patch froze R246 on save). */
static int pk_len(const pd_key *k) { return k->sound >= PERC_FACTORY_COUNT ? 8 + 42 : 8; }
static int32_t pk_put(const pd_key *k, int i)
{
    switch (i) {
    case 0: return k->sound;  case 1: return k->pitch; case 2: return k->length; case 3: return k->punch;
    case 4: return k->tone;   case 5: return k->drive; case 6: return k->level;  case 7: return k->morph;
    default: return i < 29 ? k->patch[i - 8] : k->right[i - 29];
    }
}
static void pk_get(pd_key *dst, const int32_t *a, int n)
{
    if (n < 8) return;
    pd_key k = *dst;
    k.sound = (uint8_t)(a[0] < 0 ? 0 : a[0] > 255 ? 255 : a[0]);
    k.pitch = clamp15(a[1]); k.length = clamp15(a[2]); k.punch = clamp15(a[3]);
    k.tone = clamp15(a[4]);  k.drive = clamp15(a[5]);  k.level = clamp15(a[6]); k.morph = clamp15(a[7]);
    if (n >= 50) { for (int i = 0; i < 21; i++) { k.patch[i] = clampw(a[8 + i]); k.right[i] = clampw(a[29 + i]); } }
    else if (k.sound < PERC_FACTORY_COUNT) { for (int i = 0; i < 21; i++) { k.patch[i] = PERC_FACTORY[k.sound][i]; k.right[i] = PERC_FACTORY_R[k.sound][i]; } }
    else { k.sound = 0; for (int i = 0; i < 21; i++) { k.patch[i] = PERC_FACTORY[0][i]; k.right[i] = PERC_FACTORY_R[0][i]; } }
    *dst = k;
}

static void pd_slot_loaded(int slot, void *op1r)
{
    if ((unsigned)slot >= PD_SLOTS) return;
    if (!ours(op1r)) { claimed[slot] = 0; return; }
    BCU(50);
    for (int p = 0; p < PD_PADS; p++) key_default(&kit[slot][p], p);
    const void *pk = JV_CGET(op1r, "pk");
    if (pk && jv_type(pk) == JV_ARRAY) {                      /* "pk": the compact per-key arrays */
        void *arr = JV_KEY(op1r, "pk");
        unsigned n = JV_SIZE(arr);
        for (unsigned p = 0; p < n && p < PD_PADS; p++) {
            void *e = JV_IDX(arr, p);
            unsigned m = jv_type(e) == JV_ARRAY ? JV_SIZE(e) : 0;
            if (m > 50) m = 50;
            int32_t a[50];
            for (unsigned f = 0; f < m; f++) a[f] = jv_int(JV_IDX(e, f));
            pk_get(&kit[slot][p], a, (int)m);
        }
    } else {                                                  /* "perc": the int arrays of earlier builds */
        const void *a = JV_CGET(op1r, "perc");
        if (a && jv_type(a) == JV_ARRAY) {
            void *arr = JV_KEY(op1r, "perc");
            unsigned n = JV_SIZE(arr);
            for (unsigned p = 0; p < n && p < PD_PADS; p++) {
                void *e = JV_IDX(arr, p);
                unsigned m = jv_type(e) == JV_ARRAY ? JV_SIZE(e) : 0;
                if (m < PD_NF_OLD) continue;
                if (m > PD_NF) m = PD_NF;
                pd_key *k = &kit[slot][p];
                for (unsigned f = 0; f < m; f++) key_get(k, (int)f, jv_int(JV_IDX(e, f)));
                if (m < PD_NF) { k->morph = 0; for (int i = 0; i < 21; i++) k->right[i] = k->patch[i]; }
                key_fix_right(k);
            }
        }
    }
    claimed[slot] = 1;
}
/* keys of the slot (bit per key) -> op1r.pk[key], a compact int array each (pk_put). Nothing is removed. Only while no
 * patch job serializes the patch: from inside a type job, or after op1_patch_idle() on the key thread. */
#define JV_NULL_C ((void *(*)(void *, int))0x019BFA30)        /* Value(ValueType) (patch_repair.c) */
static void kit_write(int slot, uint32_t keys)
{
    void *json = (uint8_t *)DRUM_PATCH(slot) + 4;
    void *op = JV_KEY(json, "op1r");
    if (!ours(op)) return;
    uint32_t tmp[4];
    void *arr = JV_KEY(op, "pk");
    if (jv_type(arr) != JV_ARRAY) {                           /* absent (null) or another form: a fresh array */
        JV_NULL_C(tmp, JV_ARRAY); JV_ASSIGN(arr, tmp); JV_DTOR(tmp, 2);
        keys = (1u << PD_PADS) - 1;                           /* every key, so no entry stays null */
    }
    for (int p = 0; p < PD_PADS; p++) if (keys >> p & 1) {
        const pd_key *k = &kit[slot][p];
        int len = pk_len(k);
        void *e = JV_IDX(arr, (unsigned)p);
        JV_NULL_C(tmp, JV_ARRAY); JV_ASSIGN(e, tmp); JV_DTOR(tmp, 2);   /* fresh inner array (resets its length) */
        for (int f = 0; f < len; f++) { JV_FROM_INT(tmp, pk_put(k, f)); JV_ASSIGN(JV_IDX(e, (unsigned)f), tmp); JV_DTOR(tmp, 2); }
    }
}
static volatile uint32_t dirty_keys[PD_SLOTS];               /* keys changed since kit_write (key thread only) */
/* from the key thread only (knob turns, key events via pd_sync; the stock pad writes run there too), never waits:
 * while a patch job runs the keys stay dirty for the next turn or key */
static void pd_flush(void)
{
    for (int slot = 0; slot < PD_SLOTS; slot++) {
        uint32_t k = dirty_keys[slot];
        if (!k) continue;
        if (!op1_patch_idle()) return;                        /* a save / type job runs: on the next turn or key */
        dirty_keys[slot] = 0;
        if (!claimed[slot]) continue;
        BCU(31); kit_write(slot, k); BCU(33);
        DRUM_TO_PATCH(DRUM_PATCH(slot)); BCU(34);             /* marks the patch changed: the delayed save writes it */
    }
}

/* type job: our row -> claim the sampled kit (fresh keys); another selection -> let go, then the stock job */
static void pd_type_job(void *job)
{
    int slot = *(int32_t *)((uint8_t *)job + 4), sel = *(int32_t *)((uint8_t *)job + 8);
    if ((unsigned)slot >= PD_SLOTS) { TYPE_JOB(job); return; }
    BCU(60);
    void *json = (uint8_t *)DRUM_PATCH(slot) + 4;
    uint32_t tmp[4];
    if (sel < 2) {
        JV_REMOVE(json, tmp, "op1r"); JV_DTOR(tmp, 2);
        claimed[slot] = 0;
        dpu_sync_auto();
        TYPE_JOB(job);
        return;
    }
    if (!jv_is_str(JV_KEY(json, "type"), "drum")) return;     /* dbox: not a sampled kit */
    void *op = JV_KEY(json, "op1r");                          /* null -> object on the first key */
    JV_FROM_STR(tmp, "perc"); JV_ASSIGN(JV_KEY(op, "engine"), tmp); JV_DTOR(tmp, 2);
    pd_slot_loaded(slot, op);                                 /* its keys (a perc kit from before keeps them; else fresh) */
    kit_write(slot, (1u << PD_PADS) - 1);                     /* inside the job: no patch job can overlap */
    dirty_keys[slot] = 0;
    struct { uint32_t vt; uint8_t drum, pad[3]; int32_t slot; } save = { 0, 1, { 0, 0, 0 }, slot };
    SAVE_SLOT(&save);                                         /* saved before the UI wakes (as sampler+, docs/66) */
    UI_SLOT(slot);
    UI_SIGNAL();
}
static int pd_is_plus(int slot) { return (unsigned)slot < PD_SLOTS && claimed[slot]; }

/* ---- the T1 page (0x10) for a perc kit; T2 (0x17) stays stock ---- */
#define SCREEN_CTOR     ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define SVG_LOAD        ((void *(*)(void *))0x01A503D8)
#define ITER_SCREEN_VT  ((const uint32_t *)0x019103FC)
static uint32_t pd_obj[32], pd_vt[10];
static void *pd_stock[2];
static uint8_t pd_ready;
static int32_t pd_acc[4];
static int pd_slot_ours(void) { return DRUM_MODE && claimed[DRUM_SLOT_CUR]; }
static int pd_pad(void) { int p = LAST_PAD; return p >= 0 && p < PD_PADS ? p : 0; }
static void *pd_screen_for(int id)
{
    if (!pd_ready || (id != 0x10 && id != 0x17)) return 0;
    if (id == 0x10 && pd_slot_ours()) return pd_obj;
    return pd_stock[id == 0x17];
}
static void pd_sync(int editor_open_on_10)
{
    pd_flush();                                               /* (inside a type job: not idle, so later) */
    if (!pd_ready) return;
    if (!editor_open_on_10) SCREEN_TABLE[0x10] = pd_screen_for(0x10);
    SCREEN_TABLE[0x17] = pd_screen_for(0x17);
}
static int pd_page(void) { return pd_ready && pd_slot_ours() && CURRENT_SCREEN == 0x10 && SCREEN_TABLE[0x10] == (void *)pd_obj; }
static void pd_changed(int slot, int pad, int f0, int f1)    /* the key changed: written by pd_flush */
{
    (void)f0; (void)f1;
    dirty_keys[slot] |= 1u << pad;
    pd_flush();
}
static int pd_sound_count(int slot) { int n = PERC_FACTORY_COUNT; for (int i = 0; i < PD_XFER; i++) if (xf_have[slot][i]) n = PERC_FACTORY_COUNT + i + 1; return n; }
static void pd_set_sound(int slot, int pad, int s)           /* both sides; the key's morph stays */
{
    const uint16_t *l = sound_patch(slot, s, 0), *r = sound_patch(slot, s, 1);
    if (!l) return;
    pd_key *k = &kit[slot][pad];
    for (int i = 0; i < 21; i++) { k->patch[i] = l[i]; k->right[i] = r[i]; }
    k->sound = (uint8_t)s;
}
static int pd_knob(int knob, int delta)
{
    if (!pd_page() || knob < 0 || knob > 3) return 0;
    int32_t *acc = &pd_acc[knob];
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int st = *acc / 1024;                                     /* a step per hardware click (delta 1024) */
    if (!st) return 1;
    *acc -= st * 1024;
    int slot = DRUM_SLOT_CUR, pad = pd_pad();
    pd_key *k = &kit[slot][pad];
    if (SHIFT_HELD) {
        if (knob == 0) {
            int n = pd_sound_count(slot), s = k->sound + st;
            while (s < 0) s += n;
            s %= n;
            while (!sound_patch(slot, s, 0)) s = (s + (st > 0 ? 1 : n - 1)) % n;   /* skip empty transfer slots */
            pd_set_sound(slot, pad, s);
            pd_changed(slot, pad, 0, PD_NF);
            return 1;
        }
        int16_t *w = knob == 1 ? &k->drive : knob == 2 ? &k->level : &k->morph;
        *w = clamp15(*w + st * 1024);
        int f = knob == 1 ? 25 : knob == 2 ? 26 : 28;
        pd_changed(slot, pad, f, f + 1);
        return 1;
    }
    int16_t *w = knob == 0 ? &k->pitch : knob == 1 ? &k->length : knob == 2 ? &k->punch : &k->tone;
    *w = clamp15(*w + st * (knob == 0 ? 683 : 1024));         /* pitch: a semitone a click */
    pd_changed(slot, pad, 21 + knob, 22 + knob);
    return 1;
}
static int pd_keyev(int code, int pressed)                    /* Shift + blue press: every key from the transfer */
{
    if (!pd_page() || code != 0x3B || !SHIFT_HELD) return 0;
    if (pressed) {
        int slot = DRUM_SLOT_CUR;
        BCU(70);
        for (int i = 0; i < PD_XFER && i < PD_PADS; i++)
            if (xf_have[slot][i]) { pd_set_sound(slot, i, PERC_FACTORY_COUNT + i); dirty_keys[slot] |= 1u << i; }
        BCU(71); pd_flush(); BCU(72);                         /* one write and one mark for the whole fill */
    }
    return 1;
}

static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num2(char *p, int x) { if (x < 10) *p++ = '0'; char t[4]; int n = 0; do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static void pd_init(void *scr) { (void)scr; }
static void pd_update(void *scr, int dt)
{
    int slot = DRUM_SLOT_CUR, pad = pd_pad();
#if PERC_BC
    perc_bc_tick++;
#endif
    BCU(10);
    xfer_step(slot);
    BCU(11);
    const pd_key *k = &kit[slot][pad];
    perc_view_t v;
    v.type = k->sound < PERC_FACTORY_COUNT ? k->sound : PERC_FACTORY_COUNT;
    v.has_patch = 1;
    v.k[0] = k->pitch; v.k[1] = k->length; v.k[2] = k->punch; v.k[3] = k->tone;
    v.drive = k->drive; v.level = k->level;
    v.morph = k->morph; v.morph_lane = key_sided(k) ? 1 : 2;
    static char name[20], idx[16];
    if (k->sound < PERC_FACTORY_COUNT) cat(name, PERC_FACTORY_NAME[k->sound]);
    else { char *p = cat(name, "po-32 "); num2(p, k->sound - PERC_FACTORY_COUNT + 1); }
#if PERC_BC
    { char *p = idx;
      *p++='u'; p=num2(p,(int)(perc_bc_ui%100));
      *p++='a'; p=num2(p,(int)(perc_bc_au%100));
      *p++='t'; p=num2(p,(int)(perc_bc_tick%100)); }
#else
    if (xf_slot >= 0) cat(idx, "read..");
    else { char *p = cat(idx, "key "); num2(p, pad + 1); }
#endif
    v.name = name; v.idx = idx;
    v.shift = SHIFT_HELD; v.page2[0] = v.page2[1] = v.page2[2] = 0;
    v.blue2 = "sound";
    v.hit_seq = pd_hit_seq; v.hit_vel = pd_hit_vel;
    perc_draw_page(scr, dt, &v);
    BCU(12);
}
static void pd_ui_build(void)
{
    for (int i = 0; i < 10; i++) pd_vt[i] = ITER_SCREEN_VT[i];    /* is_visible: id == current */
    pd_vt[3 * 2 + 1] = (uint32_t)pd_update;
    pd_vt[4 * 2 + 1] = (uint32_t)pd_init;
    void *saved = SCREEN_TABLE[0x10];
    SCREEN_CTOR(pd_obj, 0x10, "usbout.svg");
    SCREEN_TABLE[0x10] = saved;
    pd_obj[3] = (uint32_t)pd_vt;
}
static void pd_ui_load(void)
{
    pd_obj[0] = (uint32_t)SVG_LOAD((void *)pd_obj[2]);
    if (!pd_obj[0]) return;
    pd_init(pd_obj);
    pd_stock[0] = SCREEN_TABLE[0x10]; pd_stock[1] = SCREEN_TABLE[0x17];
    pd_ready = pd_stock[0] && pd_stock[1];
}

const op1_drum_t PERCDRUM = {
    pd_render, pd_note_on, pd_note_off, pd_slot_loaded, pd_is_plus, pd_type_job,
    pd_ui_build, pd_ui_load, pd_screen_for, pd_sync, pd_knob, pd_keyev };
#endif
