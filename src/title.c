/* title.c -- consolidated TU. */
#include "nonmatching.h"
#include "dma.h"

INCLUDE_ASM("asm/title/Func_80f2028.s");
INCLUDE_ASM("asm/title/LoadGS1TitleGFX.s");
INCLUDE_ASM("asm/title/StartTitleScreen.s");

void Func_80f2b6c(void) {}

INCLUDE_ASM("asm/title/NintendoLogo.s");
INCLUDE_ASM("asm/title/CamelotLogo.s");

void Func_80f2eb8(void) {}

INCLUDE_ASM("asm/title/Func_80f2ebc.s");

typedef enum {
    PALFADE_ALL = 0,
    PALFADE_BG = 1,
    PALFADE_OBJ = 2,
} PalFadeType;

typedef union {
    color_t colors[PALETTE_COUNT * 2]; // BGR555 in u16
    palette_t palettes[2];
    struct {
        palette_t palBG;
        palette_t palOBJ;
    };
} PaletteData;

typedef u16 BGR[3]; // aligns(2)
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
    PaletteBGRData currentBGR; // +400h
    PaletteBGRData targetBGR; // +1000h
    PaletteBGRData rampDelta;
    PaletteData paletteBuf[2];
    u8 paletteIdx;
    signed char rampFrames;
    u8 rampPos;
    u8 pad_3003;
} PaletteFadeState;

extern unsigned char iwram_3001ed0[];

static inline PaletteFadeState* GetPaletteFadeState(void) {
    return *(PaletteFadeState**)&iwram_3001ed0;
}
#if 1
INCLUDE_ASM("asm/title/Func_80f2f10.s");
extern void Func_80f2f10(void);
#else
/* Parked candidate, see src/non_matching/title/Func_80f2f10.c for notes. */
void Func_80f2f10(void)
{
    PaletteFadeState *p = GetPaletteFadeState();
    u16 *delta;
    s32 nextPos;
    u16 *cur;
    s32 i;

    delta = (u16 *)&p->rampDelta;
    if (p->rampFrames == 0)
        return;

    nextPos = p->rampPos + 1;
    p->rampPos = nextPos;
    if ((signed char)nextPos < p->rampFrames) {
        cur = (u16 *)&p->currentBGR;
        for (i = 0; i < (s32)(sizeof(PaletteBGRData) / sizeof(u16)); ++i)
            *cur++ += *delta++;
    } else {
        DMA3_COPY(&p->targetBGR, &p->currentBGR, sizeof(p->currentBGR));
        p->rampFrames = 0;
    }

    {
        u16 *src;
        u16 *dst;

        dst = (u16 *)&p->paletteBuf[p->paletteIdx ^ 1];
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
        u8 *buf = (u8 *)p + (p->paletteIdx << 10);
        ScheduleDmaTransfer(PALETTE_BG, buf + 0x2800, MAKE_DMATASK_DMA(0x8400, 0x80));
        ScheduleDmaTransfer(PALETTE_OBJ, buf + 0x2a00, MAKE_DMATASK_DMA(0x8400, 0x80));
    }
}
#endif

INCLUDE_ASM("asm/title/Func_80f3078.s");
extern void Func_80f3078(unsigned int, void*, void* target, int);

#include "task.h"
extern void *galloc_ewram(int index, unsigned int size);

void Func_80f377c(void)
{
    PaletteFadeState *p;

    p = (PaletteFadeState*)galloc_ewram(0x20, 0x3004);
    DMA3_CLEAR(p, 0x3004);
    DMA3_COPY(PALETTE_BG, (void*)&p->startPal.palBG, sizeof(p->startPal.palBG));
    DMA3_COPY(PALETTE_OBJ, (void*)&p->startPal.palOBJ, sizeof(p->startPal.palOBJ));
    Func_80f3078(0x10000, &p->startPal, &p->targetBGR, PALFADE_ALL);
    StartTask(Func_80f2f10, 0xc80);
}

extern void gfree(int index);

void Func_80f37ec(void) {
    StopTask(Func_80f2f10);
    gfree(0x20);
}

void Func_80f3804(int arg0, PalFadeType arg1) {
    PaletteFadeState *p = GetPaletteFadeState();
    if (p != NULL) {
        Func_80f3078(arg0, &p->startPal, &p->targetBGR, arg1);
    }
}


void Func_80f3824(unsigned int arg0, PalFadeType arg1) {
    PaletteFadeState *p = GetPaletteFadeState();
    if (p != NULL) {
        Func_80f3078(arg0, &p->startPal, &p->currentBGR, arg1);
    }
}

extern unsigned short *iwram_3001ed0__a2 __asm__("iwram_3001ed0");

void Func_80f3844(int arg0)
{
    unsigned short *p;
    p = iwram_3001ed0__a2;
    if (p)
        *p = arg0;
}

extern void Func_80f2ebc(void *cur, void *target, void *delta, unsigned int frames);

void Func_80f3858(unsigned int frames)
{
    PaletteFadeState *p = GetPaletteFadeState();
    if (p != NULL) {
        p->rampFrames = frames;
        p->rampPos = 0;
        Func_80f2ebc(&p->currentBGR, &p->targetBGR, &p->rampDelta, frames);
    }
}

int Func_80f3898(int arg0) {
    if (arg0 > 0x1f)
        arg0 = 0x1f;
    else if (arg0 < 0)
        arg0 = 0;
    return arg0;
}

int Func_80f38ac(int arg0) {
    int max;

    max = 0xf8 << 7;
    if (arg0 > max)
        arg0 = max;
    return arg0;
}
