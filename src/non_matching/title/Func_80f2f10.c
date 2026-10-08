/* Func_80f2f10 -- per-frame palette-fade ramp task (title screen).
 *
 * State lives in the 0x3004-byte fade work area at *iwram_3001ed0:
 *   +0x0400  current BGR555 planes  (0xC00 bytes)
 *   +0x1000  target BGR555 planes   (0xC00 bytes)
 *   +0x1c00  per-frame delta planes (0xC00 bytes)
 *   +0x2800  paletteBuf[0] (BG half at +0x000, OBJ half at +0x200)
 *   +0x2c00  paletteBuf[1]
 *   +0x3000  paletteIdx, +0x3001 rampFrames, +0x3002 rampPos
 *
 * Runs once per frame while a fade is active:
 *   1. advance rampPos; while the ramp is still running, add every delta word
 *      to the matching current word. On the final frame, DMA the target planes
 *      over the current planes instead and stop the ramp;
 *   2. pack each three-word ramp group into one BGR555 word (bits 10-14 carry
 *      the colour) and write it into the back palette buffer;
 *   3. flip the palette buffer index and queue the BG and OBJ palette uploads.
 *
 * `rampFrames` is a signed byte -- the ROM sign-extends it with ldrsb -- so
 * PaletteFadeState declares it `signed char` (a plain `char`/`s8` is unsigned
 * on this target).
 *
 * ---------------------------------------------------------------------------
 * THIS FILE IS A TWO-ZONE DOCUMENT
 * ---------------------------------------------------------------------------
 * ZONE 1 (active) is a CLEAN reconstruction: correct types and named
 *   struct-member access, written as the original authors plausibly wrote it.
 *   It deliberately contains no compiler-compensation constructs -- no
 *   `register ... __asm__` pins, no empty-asm barriers, no
 *   pin-but-never-assign tricks, no `(u32)queue + 4` / `count * 12` integer
 *   punning, no per-call `{ }` scoping games.  It is NOT byte-compatible with
 *   the ROM and is not a matching candidate; it documents the function's
 *   intended shape and shows that the ROM-vs-source gap lives in those
 *   compensation constructs, not in the algorithm.
 *   Measured: 72/154 (46.8%), 165 instructions, 384-byte symbol, literal
 *   pool 13/12 (see experiments/f2f10/RESULTS_w9_final.md).  The same
 *   algorithm with the enqueue step as a `static inline` helper instead of a
 *   macro scores 99/154, so the macro/inline choice alone is worth ~30 slots.
 *
 * ZONE 2 (disabled by default) is the FAKEMATCH candidate that scores
 *   139/154 (90.3%): 154/154 instruction count, literal pool 12/12 identical
 *   including order, 360/360 bytes, 2/2 alignment pads.  Its 15 residual
 *   mismatches are pure register-allocation / scheduling differences with no
 *   missing or extra instructions (2 prologue, 6 pack-loop preheader, 7 DMA
 *   tail).  The constructs in it are NOT original code -- they are gcc296
 *   compensation constructs, each annotated in place with what it fights and
 *   what ablating it costs (measured in experiments/f2f10, waves w6-w8).
 *
 * The two zones do NOT mirror `src/title.c`'s `#else` branch.  That branch is
 * the live parked candidate that keeps the `#if 1` / `INCLUDE_ASM` guard
 * honest, so it must stay frozen at the 139/154 form; this file's job is now
 * to document both the intended shape and the compensation campaign, which a
 * byte-for-byte mirror of the guard's `#else` half could not do.  Update
 * `src/title.c` only with a re-scored candidate; update this file when the
 * analysis changes.
 *
 * Sources of record:
 *   /workspace/goldensun/docs/Func_80f2f10-analysis.md
 *   experiments/f2f10/RESULTS_w6_macros.md   (macro vs static-inline)
 *   experiments/f2f10/RESULTS_w7_hoisted.md  (hoisted acquisition / scoping)
 *   experiments/f2f10/RESULTS_w8_shared.md   (shared macro with video.c)
 *   experiments/f2f10/RESULTS_w9_final.md    (final landing decision)
 * ---------------------------------------------------------------------------
 */

/* Compile-check switch (default 1 = ZONE 1 only).  Verify BOTH zones with the
 * repository's real toolchain contract -- `xgcc -c` does not work here (it
 * drives the system `as`, which rejects `-marm7tdmi`):
 *
 *   tools/gcc296/xgcc -Btools/gcc296/ -O2 -mthumb -mthumb-interwork \
 *     -mcpu=arm7tdmi -fno-builtin -nostdinc -ffreestanding -fcall-used-r4 \
 *     -Iinclude -fno-strict-aliasing [-DW9_ZONE_CLEAN=0] -S -o /tmp/z.s \
 *     src/non_matching/title/Func_80f2f10.c
 *   printf '\n\t.text\n\t.align\t2, 0\n' >> /tmp/z.s
 *   arm-none-eabi-as -mcpu=arm7tdmi -mthumb-interwork -Iinclude -o /tmp/z.o /tmp/z.s
 *
 * ZONE 1 -> text=384, ZONE 2 (-DW9_ZONE_CLEAN=0) -> text=360.
 */
#ifndef W9_ZONE_CLEAN
#define W9_ZONE_CLEAN 1
#endif

/* ===========================================================================
 * Shared declarations
 * ===========================================================================
 *
 * `PaletteFadeState` and its transitive types are defined in `src/title.c`,
 * not in a header, so they are mirrored here to let this parked file compile
 * on its own.  (The "does not mirror src/title.c" note at the top is about
 * *code*: those are only local typedefs, and they must stay in step with
 * src/title.c:38-57.)
 *
 * The DMA queue types come from `include/dma.h` and are already correct for
 * this function: `DmaQueue { u16 count; u16 _pad; DMATask tasks[32]; }` with
 * `DMATask { const void *src; void *dest; u32 control; }`.  The slot address
 * is therefore `&gDMATaskCount.tasks[gDMATaskCount.count]` -- exactly the
 * ROM's `(u32)queue + 4 + count * 12`, with no integer punning.  `count` is
 * `u16`: the ROM reads it with `ldrh r2,[r5]` and writes it with
 * `strh r2,[r5]`, and the 12-byte slot stride is that of a 3-word `DMATask`.
 */

#include "gba/types.h"
#include "gba/io.h"
#include "dma.h"

typedef enum {
    PALFADE_ALL = 0,
    PALFADE_BG = 1,
    PALFADE_OBJ = 2,
} PalFadeType;

typedef union {
    color_t colors[PALETTE_COUNT * 2]; /* BGR555 */
    palette_t palettes[2];
    struct {
        palette_t palBG;
        palette_t palOBJ;
    };
} PaletteData;

typedef u16 BGR[3]; /* aligns(2) */
typedef struct {
    BGR bgr[PALETTE_COUNT];
} PaletteBGR;

typedef union {
    BGR colors[PALETTE_COUNT * 2];
    PaletteBGR palettes[2];
    struct {
        PaletteBGR bgrBG;
        PaletteBGR bgrOBJ;
    };
} PaletteBGRData;

typedef struct {
    PaletteData startPal;
    PaletteBGRData currentBGR; /* +0x400 */
    PaletteBGRData targetBGR;  /* +0x1000 */
    PaletteBGRData rampDelta;  /* +0x1c00 */
    PaletteData paletteBuf[2]; /* +0x2800 */
    u8 paletteIdx;             /* +0x3000 */
    signed char rampFrames;    /* +0x3001, read with ldrsb */
    u8 rampPos;                /* +0x3002 */
    u8 pad_3003;
} PaletteFadeState;

extern unsigned char iwram_3001ed0[];

static inline PaletteFadeState *GetPaletteFadeState(void) {
    return *(PaletteFadeState **)&iwram_3001ed0;
}

#if W9_ZONE_CLEAN /* W9_ZONE:1 */
/* ===========================================================================
 * ZONE 1 -- clean, type-correct reconstruction (active)
 * ===========================================================================
 *
 * This is the function as it plausibly read before it was bent to satisfy
 * gcc 2.96: real struct members, the real 16-bit queue count, no register
 * pins, no inline-asm barriers, no integer punning.
 *
 * It is *not* binary-exact.  Measured with experiments/f2f10/score.py:
 *   72/154 instruction slots, 165 instructions, 384-byte symbol.
 * The very same body with the enqueue step written as a `static inline`
 * helper instead of a macro scores 99/154, i.e. the bulk of the remaining
 * gap is the gcc296 compensation machinery preserved in ZONE 2 below, not
 * the shape of this code.  See docs/Func_80f2f10-analysis.md for the ROM
 * contract and the w6/w7/w8 result reports for the full campaign.
 *
 * The enqueue step is kept as a macro, in the shape of the repo's existing
 * `SetRegAnimDest(queue, dest, src)` (src/maps/title.c:209-228).  See also
 * the author's own note in include/dma.h:48 and :67, wondering whether the
 * DMA helpers "were macros instead of inline functions".
 *
 * Macro parameters are named `dma_*` so that they cannot capture the
 * `DMATask` member names (`slot->src`, ...) while the body is substituted. */
#define SCHEDULE_DMA(dma_dest, dma_src, dma_control) do { \
    u16 savedIme = REG_IME; \
    SET_IO(REG_IME, REG_ADDR_IME); \
    if (gDMATaskCount.count < 32) { \
        DMATask *slot = &gDMATaskCount.tasks[gDMATaskCount.count]; \
        slot->src = (const void *)(dma_src); \
        slot->dest = (void *)(dma_dest); \
        slot->control = (dma_control); \
        gDMATaskCount.count++; \
    } \
    SET_IO(REG_IME, savedIme); \
} while (0)

void Func_80f2f10(void)
{
    PaletteFadeState *p = GetPaletteFadeState();
    signed char *fp;        /* &p->rampFrames: the ROM keeps a pointer to it */
    u16 *delta;             /* rampDelta as a flat half-word stream */
    u16 *cur;               /* currentBGR as a flat half-word stream */
    s32 nextPos;
    s32 i;

    fp = &p->rampFrames;
    delta = &p->rampDelta.colors[0][0];

    if (*fp == 0)
        return;

    nextPos = p->rampPos + 1;
    p->rampPos = nextPos;
    if ((signed char)nextPos < *fp) {
        /* Ramp still running: half-word-wise current += step.  The ROM walks
         * `sizeof(PaletteBGRData) / sizeof(u16)` half-words. */
        cur = &p->currentBGR.colors[0][0];
        for (i = 0; i < (s32)(sizeof(PaletteBGRData) / sizeof(u16)); ++i)
            *cur++ += *delta++;
    } else {
        /* Final frame: snap the target planes over the current ones. */
        DMA3_COPY(&p->targetBGR, &p->currentBGR, sizeof(PaletteBGRData));
        p->rampFrames = 0;
    }

    {
        /* Pack each three-half-word ramp group into one BGR555 half-word.
         * Read via the named union members, write via the named buffer. */
        u16 *src = &p->currentBGR.colors[0][0];
        u16 *dst = p->paletteBuf[p->paletteIdx ^ 1].colors;

        i = (s32)(sizeof(PaletteBGRData) / sizeof(u16) / 3);
        do {
            *dst++ = (src[0] & 0x7C00)
                   | (((src[1] << 16) >> 21) & 0x3E0)
                   | (((src[2] << 16) >> 26) & 0x1F);
            src += 3;
        } while (--i != 0);
    }

    p->paletteIdx ^= 1;

    /* Queue the back-buffer uploads for the *new* front buffer: BG, then OBJ.
     * DmaQueue::count is `u16` (include/dma.h:142-146), which is what the ROM's
     * `ldrh`/`strh` on it require; the 0x80 word count is the 128 half-words
     * of one palette plane. */
    SCHEDULE_DMA(PALETTE_BG, &p->paletteBuf[p->paletteIdx].palBG,
                 MAKE_DMATASK_DMA(0x8400, 0x80));
    SCHEDULE_DMA(PALETTE_OBJ, &p->paletteBuf[p->paletteIdx].palOBJ,
                 MAKE_DMATASK_DMA(0x8400, 0x80));
}

#endif /* W9_ZONE:1 */

/* ===========================================================================
 * ZONE 2 -- fakematch candidate (disabled by default)
 * ===========================================================================
 *
 * The 139/154 source, kept verbatim.  NOT original code: every pin, barrier
 * and scope trick below exists only to steer gcc 2.96 into the ROM's
 * instruction schedule.  Compile-check it with `-DW9_ZONE_CLEAN=0`.
 */
#if !W9_ZONE_CLEAN /* W9_ZONE:2 */

/* --- ScheduleDmaA / ScheduleDmaB (gcc296 compensation, NOT original) -------
 * include/dma.h:48 guesses these were all one macro; wave w8 tested sharing the
 * macro text with src/video.c and found the two TUs need mutually exclusive
 * constructs (video needs d/s pinned r6/r0 + REG_IME form, title needs the
 * opposite), ceiling 125/154 -- see RESULTS_w8_shared.md.  Wave w6 macro-ised
 * them in place: B alone as a macro keeps 139/154 exactly (`w6_mx2`), A as a
 * macro costs exactly 1 slot (138/154, `w6_m4`) because gcc emits
 * `movs r1,#160; lsls r1,#6` one slot later.  See RESULTS_w6_macros.md. */
static inline void ScheduleDmaA(void* dest, u8* base, u32 control) {
    /* pin queue->r5, ime->r4: the ROM keeps the queue base in r5 and the IME
     * address in r4 live across BOTH calls (no reload).  Unpinned, they land
     * in r6/r0/r7 and the whole tail is re-allocated. */
    register DmaQueue* queue __asm__("r5");
    register vu16* ime __asm__("r4");
    u32 savedIme;
    /* off pinned to r1 so the 0x2800 materialisation lands in the ROM slot
     * (`movs r1,#160; lsls r1,#6`).  Declaring it at the top of the helper
     * (not inside the `if`) keeps it live across the branch. */
    register u32 off __asm__("r1") = 0x2800;
    u32 s;
    s32 count;
    u32* task;
    queue = &gDMATaskCount;
    ime = (vu16 *)REG_ADDR_IME;
    /* empty-asm barrier on `queue` (the idiom from src/video.c:38): stops gcc
     * CSE-ing the 0x84000080 control word into r7, so it is reloaded per call
     * as the ROM does.  Ablating it costs an instruction (w6). */
    __asm__ volatile("" : : "r"(queue));
    s = (u32)base + off;
    /* `off` liveness barrier: forces the ROM's separate saved-IME copy
     * `ldrh r3,[r4]; adds r1,r3,#0` instead of a coalesced `ldrh r1,[r4]`. */
    __asm__ volatile("" : : "r"(off));
    savedIme = *ime;
    *ime = (u16)(u32)ime;
    count = queue->count;
    if (count < 32) {
        task = (u32*)(count * 12 + (u32)queue + 4);
        *task++ = s;
        queue->count = count + 1;
        *task++ = (u32)dest;
        *task = control;
    }
    *ime = (u16)savedIme;
}

/* ScheduleDmaB -- the second call reuses the r5/r4 values A left live.
 * The pin-but-never-assign below is deliberate: wave w7 h99 ablation shows
 * adding an assignment costs exactly 2 slots (139 -> 137).  It is the
 * fakematch this file documents, not something to "fix". */
static inline void ScheduleDmaB(void* dest, u8* base, u32 control) {
    /* pin queue->r5, ime->r4: the ROM keeps the queue base in r5 and the IME
     * address in r4 live across BOTH calls (no reload).  Unpinned, they land
     * in r6/r0/r7 and the whole tail is re-allocated. */
    register DmaQueue* queue __asm__("r5");
    register vu16* ime __asm__("r4");
    u32 savedIme;
    register u32 off __asm__("r1");
    u32 s;
    s32 count;
    u32* task;
    /* empty-asm barrier on `queue` (the idiom from src/video.c:38): stops gcc
     * CSE-ing the 0x84000080 control word into r7, so it is reloaded per call
     * as the ROM does.  Ablating it costs an instruction (w6). */
    __asm__ volatile("" : : "r"(queue));
    savedIme = *ime;
    *ime = (u16)(u32)ime;
    count = queue->count;
    if (count < 32) {
        /* 0x2a00 is materialised only inside the `if`, matching the ROM (dead
         * on the count==32 path). */
        off = 0x2a00;
        s = (u32)base + off;
        __asm__ volatile("" : : "r"(off));
        task = (u32*)(count * 12 + (u32)queue + 4);
        queue->count = count + 1;
        /* second `queue` barrier: keeps the store order (count, then src, then
         * dest/control) and the per-call control-word reload.  Wave w7b
         * measured that sharing these work locals across both calls instead of
         * per-call scoping costs 17 slots (138 -> 121). */
        __asm__ volatile("" : : "r"(queue));
        *task++ = s;
        *task++ = (u32)dest;
        *task = control;
    }
    *ime = (u16)savedIme;
}

/* -------------------------------------------------------------------------
 * ZONE 2 body -- construct notes for the non-helper parts:
 *  - `fp` declared and assigned BEFORE `delta`: this schedules the 0x3001 pool
 *    load before the `p` load, which is what makes the ROM's 12-word pool
 *    (&rampPos via `adds r2,#1` off the 0x3001 constant) reachable at all.
 *  - `doff` pinned r1 + its barrier materialises 0x1c00 in the ROM slot.
 *  - reusing `doff` for 0x3000 plus the `"+r"(aptr)` read-write barrier keeps
 *    the paletteIdx `ldrb` from folding its address into the load.
 *  - the else-branch `b1` r1 = 0x1000 / `b2` r2 = 0x400 are both pinned and
 *    simultaneously live behind one barrier, reproducing the ROM's parallel
 *    constant materialisation around the DMA3 burst.
 *  - the DMA tail sits in a per-call `{ }` block: sharing the work locals
 *    across both calls scores 121/154 (wave w7b), so per-expansion pseudo
 *    lifetime is worth 17 slots (138 -> 121).
 *  - the residual 15 mismatches (2 prologue, 6 pack preheader, 7 DMA tail)
 *    are gcc296-vs-2.95 scheduler/allocator artifacts; an asm-level reorder
 *    of the exact prologue scores 99/154, so they are not reachable from C.
 *
 * The body below is byte-for-byte the landed candidate except for the
 * function name: it is `static` and suffixed `_zone2` so that the repository
 * structure checker (tools/c_source.py `parse_funcs`, a lexical reader that
 * does not evaluate `#if`) does not report two definitions of Func_80f2f10
 * in one file.  When compiling this zone, the name is irrelevant -- only the
 * instruction stream matters.
 * ------------------------------------------------------------------------- */
static void Func_80f2f10_zone2(void)
{
    PaletteFadeState *p = GetPaletteFadeState();
    signed char *fp;
    u16 *delta;
    s32 nextPos;
    u16 *cur;
    s32 i;
    register u8 *aptr __asm__("r3");
    register u8 pidxv __asm__("r2");
    register u32 doff __asm__("r1") = 0x1c00;
    __asm__ volatile("" : : "r"(doff));
    delta = (u16 *)((u8 *)p + doff);
    fp = &p->rampFrames;

    if (*fp == 0)
        return;

    nextPos = p->rampPos + 1;
    p->rampPos = nextPos;
    if ((signed char)nextPos < *fp) {
        cur = (u16 *)&p->currentBGR;
        for (i = 0; i < (s32)(sizeof(PaletteBGRData) / sizeof(u16)); ++i)
            *cur++ += *delta++;
    } else {
        register u32 b1 __asm__("r1") = 0x1000;
        register u32 b2 __asm__("r2") = 0x400;
        __asm__ volatile("" : : "r"(b1), "r"(b2));
        DMA3_COPY((u8 *)p + b1, (u8 *)p + b2, 0xC00);
        p->rampFrames = 0;
    }

    doff = 0x3000;
    __asm__ volatile("" : : "r"(doff));
    aptr = (u8 *)p + doff;
    __asm__ volatile("" : "+r"(aptr));
    pidxv = *aptr;

    {
        u16 *src;
        u16 *dst;

        dst = (u16 *)&p->paletteBuf[pidxv ^ 1];
        src = (u16 *)&p->currentBGR;
        i = sizeof(PaletteBGRData) / sizeof(u16) / 3;

        do {
            *dst++ = (src[0] & 0x7C00) | (((src[1] << 16) >> 21) & 0x3E0)
                   | (((src[2] << 16) >> 26) & 0x1F);
            src += 3;
        } while (--i != 0);
    }

    p->paletteIdx ^= 1;
    {
        /* paletteBuf[paletteIdx]; the +0x2800/+0x2A00 BG/OBJ halves are
           added inside the ScheduleDmaA/B helpers. */
        u8 *buf = (u8 *)p + (p->paletteIdx << 10);
        ScheduleDmaA(PALETTE_BG, buf, MAKE_DMATASK_DMA(0x8400, 0x80));
        ScheduleDmaB(PALETTE_OBJ, buf, MAKE_DMATASK_DMA(0x8400, 0x80));
    }
}

#endif /* W9_ZONE:2 */
