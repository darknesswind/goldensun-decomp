/* title.c -- consolidated TU. */
#include "nonmatching.h"

INCLUDE_ASM("asm/title/Func_80f2028.s");
INCLUDE_ASM("asm/title/LoadGS1TitleGFX.s");
INCLUDE_ASM("asm/title/StartTitleScreen.s");

void Func_80f2b6c(void) {}

INCLUDE_ASM("asm/title/NintendoLogo.s");
INCLUDE_ASM("asm/title/CamelotLogo.s");

void Func_80f2eb8(void) {}

INCLUDE_ASM("asm/title/Func_80f2ebc.s");
INCLUDE_ASM("asm/title/Func_80f2f10.s");
INCLUDE_ASM("asm/title/Func_80f3078.s");

#include "dma.h"
#include "task.h"
extern void *galloc_ewram(int index, unsigned int size);
extern void Func_80f3078(unsigned int, void*, void* target, int);
extern void Func_80f2f10(void);

typedef enum {
	PALFADE_ALL = 0,
	PALFADE_BG = 1,
	PALFADE_OBJ = 2,
} PalFadeType;

typedef struct {
	color_t planeB[256];
	color_t planeG[256];
	color_t planeR[256];
} PalettePlanes;

typedef struct {
	palette_t palBG;
	palette_t palOBJ;
} PaletteData;

typedef struct {
    PalettePlanes palnBG;
    PalettePlanes palnOBJ;
} PalettePlanesData;

typedef struct {
    PaletteData startPal;
    PalettePlanesData curPlanes; // +400h
    PalettePlanesData targetPlanes; // +1000h
	u16 rampDelta[3][512];
	u16 dmaMirror[1024];
	u8 mirrorIndex;
	char rampFrames;
	u8 rampPos;
	u8 pad_3003;
} PaletteFadeState;

void Func_80f377c(void)
{
    PaletteFadeState *p;

    p = (PaletteFadeState*)galloc_ewram(0x20, 0x3004);
    DMA3_CLEAR(p, 0x3004);
    DMA3_COPY(PALETTE_BG, (void*)&p->startPal.palBG, sizeof(p->startPal.palBG));
    DMA3_COPY(PALETTE_OBJ, (void*)&p->startPal.palOBJ, sizeof(p->startPal.palOBJ));
    Func_80f3078(0x10000, &p->startPal, &p->targetPlanes, PALFADE_ALL);
    StartTask(Func_80f2f10, 0xc80);
}

extern void gfree(int index);

void Func_80f37ec(void) {
    StopTask(Func_80f2f10);
    gfree(0x20);
}

extern unsigned char iwram_3001ed0[];

static inline PaletteFadeState* GetPaletteFadeState(void) {
	return *(PaletteFadeState**)&iwram_3001ed0;
}

void Func_80f3804(int arg0, PalFadeType arg1) {
    PaletteFadeState *p = GetPaletteFadeState();
    if (p != NULL) {
        Func_80f3078(arg0, &p->startPal, &p->targetPlanes, arg1);
    }
}


void Func_80f3824(unsigned int arg0, PalFadeType arg1) {
    PaletteFadeState *p = GetPaletteFadeState();
    if (p != NULL) {
        Func_80f3078(arg0, &p->startPal, &p->curPlanes, arg1);
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
        Func_80f2ebc(&p->curPlanes, &p->targetPlanes, &p->rampDelta, frames);
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
