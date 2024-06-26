
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <malloc.h>
#include <fcntl.h>
#include <math.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/dos/emm.h>
#include <hw/dos/himemsys.h>
#include <hw/vga/vga.h>
#include <hw/vga/vrl.h>
#include <hw/8254/8254.h>
#include <hw/8259/8259.h>
#include <fmt/minipng/minipng.h>

#include "timer.h"
#include "vmode.h"
#include "fonts.h"
#include "vrlimg.h"
#include "dbgheap.h"
#include "fontbmp.h"
#include "unicode.h"
#include "commtmp.h"
#include "sin2048.h"
#include "vrldraw.h"
#include "seqcomm.h"
#include "keyboard.h"
#include "dumbpack.h"
#include "fzlibdec.h"
#include "fataexit.h"
#include "sorcpack.h"
#include "rotozoom.h"
#include "seqcanvs.h"
#include "cutscene.h"

#include <hw/8042/8042.h>

#define GAME_PAL_TEXT       (255u)

struct game_2dvec_t {
    int32_t         x,y;    /* 16.16 fixed point */
};

unsigned                    game_flags;
#define GF_CHEAT_NOCLIP     (1u << 0u)

#define noclip_on()         ((game_flags & GF_CHEAT_NOCLIP) != 0u)

struct game_2dvec_t         game_position;
uint16_t                    game_angle;
int                         game_current_room;
uint8_t                     game_minigame_select;

#define GAME_VERTICES       128
struct game_2dvec_t         game_vertex[GAME_VERTICES];
struct game_2dvec_t         game_vertexrot[GAME_VERTICES];
unsigned                    game_vertex_max;

/* from start to end, the wall is a line and faces 90 degrees to the right from the line, DOOM style.
 *
 *       ^
 *       |
 *   end +
 *       |
 *       |
 *       |-----> faces this way (sidedef 0)
 *       |
 *       |
 * start +
 */
struct game_2dlineseg_t {
    uint16_t                start,end;                  /* vertex indices */
    uint16_t                flags;
    uint16_t                sidedef[2];                 /* [0]=front side       [1]=opposite side */
};

#define GAME_LINESEG        128
struct game_2dlineseg_t     game_lineseg[GAME_LINESEG];
unsigned                    game_lineseg_max;

struct game_2dsidedef_t {
    uint8_t                 texture;
    unsigned                texture_render_w;
};

#define GAME_SIDEDEFS       128
struct game_2dsidedef_t     game_sidedef[GAME_SIDEDEFS];
unsigned                    game_sidedef_max;

/* No BSP tree, sorry. The 3D "overworld" is too simple and less important to need it.
 * Also no monsters and cacodemons to shoot. */

#define GAME_TEXTURE_W      64
#define GAME_TEXTURE_H      64
struct game_2dtexture_t {
    unsigned char*          tex;        /* 64x64 texture = 2^6 * 2^6 = 2^12 = 4096 bytes = 4KB */
};

#define GAME_TEXTURES       8
struct game_2dtexture_t     game_texture[GAME_TEXTURES];

struct game_door_t {
    uint16_t                open;               /* door open (0xFFFF) or close (0x0000) state */
    int16_t                 open_speed;         /* door movement */
    unsigned                door_lineseg;       /* lineseg that defines the door (that swings) */
    unsigned                origrot_vertex;     /* copy of the original vertex that is rotated for the door anim (this is modified on load) */
};

#define GAME_DOORS          8
struct game_door_t          game_door[GAME_DOORS];
unsigned                    game_door_max;

enum {
    GTT_NONE=0,
    GTT_DOOR,
    GTT_TEXT,
    GTT_MINIGAME
};

#define GTF_TRIGGER_ON      (1u << 0u)

struct game_trigger_t {
    struct game_2dvec_t     tl,br;              /* bounding box */
    uint8_t                 type;
    uint8_t                 flags;
    unsigned                door;               /* game door */
    const char*             msg;
};

#define GAME_TRIGGERS       16
struct game_trigger_t       game_trigger[GAME_TRIGGERS];
unsigned                    game_trigger_max;

struct game_vslice_t {
    int16_t                 floor,ceil;         /* wall slice (from floor to ceiling) */
    unsigned                sidedef;
    unsigned                flags;
    unsigned                next;               /* next to draw or ~0u */
    uint8_t                 tex_n;
    uint8_t                 tex_x;
    int32_t                 dist;
};

#define VSF_TRANSPARENT     (1u << 0u)

/* we have to allocate this so that Open Watcom doesn't stick the array into FAR_DATA which then
 * bloats the EXE file with 0x8000 bytes of zeros */
#define GAME_VSLICE_MAX     2048
struct game_vslice_t*       game_vslice;
unsigned                    game_vslice_alloc;

#define GAME_VSLICE_DRAW    320
unsigned                    game_vslice_draw[GAME_VSLICE_DRAW];

#define GAME_MIN_Z          (1l << 14l)

#define GAMETEX_LOAD_PAL0   (1u << 0u)

struct game_text_char_t {
    uint16_t                c;
    uint16_t                x,y;
};

#define GAME_TEXT_CHAR_MAX  256
struct game_text_char_t     game_text_char[GAME_TEXT_CHAR_MAX];
unsigned                    game_text_char_max;

void game_texture_load(const unsigned i,const char *path,const unsigned f) {
    struct minipng_reader *rdr;
    unsigned char *row;
    unsigned int ri;

    if (game_texture[i].tex != NULL)
        return;

    if ((rdr=minipng_reader_open(path)) == NULL)
        fatal("gametex png error %s",path);
    if (minipng_reader_parse_head(rdr) || rdr->plte == NULL || rdr->plte_count == 0 || rdr->ihdr.width != 64 || rdr->ihdr.height != 64 || rdr->ihdr.bit_depth != 8)
        fatal("gametex png error %s",path);
    if ((game_texture[i].tex=malloc(64*64)) == NULL)
        fatal("gametex png error %s",path);
    if ((row=malloc(64)) == NULL)
        fatal("gametex png error %s",path);

    for (ri=0;ri < 64;ri++) {
        if (minipng_reader_read_idat(rdr,row,1) != 1) /* pad byte */
            fatal("gametex png error %s",path);
        if (minipng_reader_read_idat(rdr,row,64) != 64) /* row */
            fatal("gametex png error %s",path);

        {
            unsigned int x;
            unsigned char *srow = row;
            unsigned char *drow = game_texture[i].tex + ri;

            for (x=0;x < 64;x++) {
                *drow = *srow++;
                drow += 64;
            }
        }
    }

    if (f & GAMETEX_LOAD_PAL0) {
        unsigned char *pal = (unsigned char*)rdr->plte;
        unsigned int x;

        vga_palette_lseek(0);
        for (x=0;x < rdr->plte_count;x++) vga_palette_write(pal[x*3+0]>>2,pal[x*3+1]>>2,pal[x*3+2]>>2);
    }

    minipng_reader_close(&rdr);
    free(row);
}

int32_t game_3dto2d(struct game_2dvec_t *d2) {
    const int32_t dist = d2->y >> 2l;

    d2->x = ((int32_t)(160l << 16l)) + (int32_t)(((int64_t)d2->x << (16ll + 6ll)) / (int64_t)dist); /* fixed point 16.16 division */
    d2->y = 100l << 16l;

    return dist;
}

#define TEXPRECIS               (0)
#define ZPRECSHIFT              (8)
void game_project_lineseg(const unsigned int i) {
    struct game_2dlineseg_t *lseg = &game_lineseg[i];
    struct game_2dsidedef_t *sdef;

    /* 3D to 2D project */
    /* if the vertices are backwards, we're looking at the opposite side */
    {
        unsigned sidedef;
        struct game_2dvec_t pr1,pr2;
        int32_t od1,od2;
        int32_t  u1,u2;
        int32_t d1,d2;
        unsigned side;
        int x1,x2,x;
        int ix,ixd;

        u1 = 0;
        u2 = 0x10000ul;
        pr1 = game_vertexrot[lseg->start];
        pr2 = game_vertexrot[lseg->end];
        side = 0;

        if (pr1.y < GAME_MIN_Z && pr2.y < GAME_MIN_Z) {
            return;
        }
        else if (pr1.y < GAME_MIN_Z || pr2.y < GAME_MIN_Z) {
            const int32_t dx = pr2.x - pr1.x;
            const int32_t dy = pr2.y - pr1.y;

            if (labs(dx) < labs(dy)) {
                if (pr2.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr1.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u2 = (int32_t)(((int64_t)u2 * (int64_t)cdy) / (int64_t)dy);
                    pr2.x = pr1.x + cdx;
                    pr2.y = GAME_MIN_Z;
                }
                else if (pr1.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr2.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u1 = 0x10000l - (int32_t)(((int64_t)(0x10000l - u1) * (int64_t)-cdy) / (int64_t)dy);
                    pr1.x = pr2.x + cdx;
                    pr1.y = GAME_MIN_Z;
                }
                else {
                    return;
                }
            }
            else {
                if (pr2.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr1.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u2 = (int32_t)(((int64_t)u2 * (int64_t)cdx) / (int64_t)dx);
                    pr2.x = pr1.x + cdx;
                    pr2.y = GAME_MIN_Z;
                }
                else if (pr1.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr2.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u1 = 0x10000l - (int32_t)(((int64_t)(0x10000l - u1) * (int64_t)-cdx) / (int64_t)dx);
                    pr1.x = pr2.x + cdx;
                    pr1.y = GAME_MIN_Z;
                }
                else {
                    return;
                }
            }
        }

        d1 = game_3dto2d(&pr1);
        d2 = game_3dto2d(&pr2);

        if (pr1.x > pr2.x) {
            struct game_2dvec_t tpr;
            uint16_t tu;
            int32_t td;

            tpr = pr1;
            td = d1;
            tu = u1;

            pr1 = pr2;
            d1 = d2;
            u1 = u2;

            pr2 = tpr;
            d2 = td;
            u2 = tu;

            side = 1;
        }

        if ((sidedef=lseg->sidedef[side]) == (~0u))
            return;

        sdef = &game_sidedef[sidedef];

        od1 = d1;
        d1 = (1l << (32l - (int32_t)ZPRECSHIFT)) / (int32_t)d1;         /* d1 = 1/z1 */

        od2 = d2;
        d2 = (1l << (32l - (int32_t)ZPRECSHIFT)) / (int32_t)d2;         /* d2 = 1/z2 */

        ix = x1 = (int)(pr1.x >> 16l);
        if (x1 < 0) x1 = 0;

        x2 = (int)(pr2.x >> 16l);
        ixd = x2 - ix;
        if (x2 > 320) x2 = 320;

        u1 = (int32_t)((((int64_t)sdef->texture_render_w << (int64_t)TEXPRECIS) * (int64_t)u1) / (int64_t)od1);
        u2 = (int32_t)((((int64_t)sdef->texture_render_w << (int64_t)TEXPRECIS) * (int64_t)u2) / (int64_t)od2);

        for (x=x1;x < x2;x++) {
            if (game_vslice_alloc < GAME_VSLICE_MAX) {
#if 1/*ASM*/
                const unsigned pri = game_vslice_draw[x];
                int32_t id,d;

                /* id = d1 + (((d2 - d1) * (x - ix)) / ixd); */     /* interpolate between 1/z1 and 1/z2 */
                __asm {
                    .386
                    mov     ax,x
                    sub     ax,ix
                    movsx   eax,ax                  ; eax = x - ix
                    mov     ebx,d2
                    sub     ebx,d1                  ; ebx = d2 - d1
                    imul    ebx                     ; edx:eax = (x - ix) * (d2 - d1)
                    mov     bx,ixd
                    movsx   ebx,bx                  ; ebx = ixd
                    idiv    ebx                     ; eax = ((x - ix) * (d2 - d1)) / ixd
                    add     eax,d1                  ; eax = u1 + (((x - ix) * (d2 - d1)) / ixd)
                    mov     id,eax
                }
                /* d = (1l << (32l - (int32_t)ZPRECSHIFT)) / id; */ /* d = 1 / id */
                __asm {
                    .386
                    xor     edx,edx
                    mov     eax,0x1000000           ; 1 << (32 - 8) = 1 << 24 = 0x01000000
                    idiv    id
                    mov     d,eax
                }
#else
                const int32_t id = d1 + (((d2 - d1) * (x - ix)) / ixd);     /* interpolate between 1/z1 and 1/z2 */
                const int32_t d = (1l << (32l - (int32_t)ZPRECSHIFT)) / id; /* d = 1 / id */
                const unsigned pri = game_vslice_draw[x];
#endif

                if (pri != (~0u)) {
                    if (d > game_vslice[pri].dist) {
                        if (!(game_vslice[pri].flags & VSF_TRANSPARENT))
                            continue;
                    }
                }

                {
#if 1/*ASM*/
                    const unsigned vsi = game_vslice_alloc++;
                    struct game_vslice_t *vs = &game_vslice[vsi];
                    int32_t tid,tx;
                    int h;

                    /* tid = u1 + (((u2 - u1) * (x - ix)) / ixd); */     /* interpolate between 1/u1 and 1/u2 (texture mapping) */
                    __asm {
                        .386
                        mov     ax,x
                        sub     ax,ix
                        movsx   eax,ax                  ; eax = x - ix
                        mov     ebx,u2
                        sub     ebx,u1                  ; ebx = u2 - u1
                        imul    ebx                     ; edx:eax = (x - ix) * (u2 - u1)
                        movsx   ebx,ixd                 ; ebx = ixd
                        idiv    ebx                     ; eax = ((x - ix) * (u2 - u1)) / ixd
                        add     eax,u1                  ; eax = u1 + (((x - ix) * (u2 - u1)) / ixd)
                        mov     tid,eax
                    }
                    /* tx = (tid << (16l - (int32_t)ZPRECSHIFT)) / id; */ /* texture map u coord = 1 / tid */
                    __asm {
                        .386
                        xor     edx,edx
                        mov     eax,tid
                        mov     cl,8                ; (16 - 8)
                        shl     eax,cl
                        idiv    id
                        mov     tx,eax
                    }
                    /* h = (64l << 16l) / d; */
                    __asm {
                        .386
                        xor     edx,edx
                        mov     eax,0x400000        ; (64l << 16l) = 0x40 << 16l = 0x400000
                        idiv    d
                        mov     h,ax
                    }
#else/*C*/
                    const int32_t tid = u1 + (((u2 - u1) * (x - ix)) / ixd);      /* interpolate between 1/u1 and 1/u2 (texture mapping) */
                    const int32_t tx = (tid << (16l - (int32_t)ZPRECSHIFT)) / id; /* texture map u coord = 1 / tid */
                    const int h = (int)((64l << 16l) / d);
                    const unsigned vsi = game_vslice_alloc++;
                    struct game_vslice_t *vs = &game_vslice[vsi];
#endif

                    vs->flags = 0;
                    vs->sidedef = sidedef;
                    vs->ceil = (int)(((100 << 1) - h) >> 1);
                    vs->floor = (int)(((100 << 1) + h) >> 1);

                    if (vs->flags & VSF_TRANSPARENT)
                        vs->next = pri;
                    else
                        vs->next = (~0u);

                    vs->dist = d;
                    vs->tex_n = sdef->texture;
                    vs->tex_x = (tx >> TEXPRECIS) & 0x3Fu;

                    game_vslice_draw[x] = vsi;
                }
            }
        }
    }
}
#undef ZPRECSHIFT

void game_texture_free(struct game_2dtexture_t *t) {
    if (t->tex != NULL) {
        free(t->tex);
        t->tex = NULL;
    }
}

void game_texture_freeall(void) {
    unsigned int i;

    for (i=0;i < GAME_TEXTURES;i++)
        game_texture_free(&game_texture[i]);
}

struct game_room_bound {
    struct game_2dvec_t             tl,br;      /* top left, bottom right */

    unsigned                        vertex_count;
    const struct game_2dvec_t*      vertex;

    unsigned                        lineseg_count;
    const struct game_2dlineseg_t*  lineseg;

    unsigned                        sidedef_count;
    const struct game_2dsidedef_t*  sidedef;

    const struct game_room_bound**  also;       /* adjacent rooms to check boundaries as well and render */

    unsigned                        door_count;
    const struct game_door_t*       door;

    unsigned                        trigger_count;
    const struct game_trigger_t*    trigger;
};

/* NTS: When 'x' is float, you cannot do x << 16 but you can do x * 0x10000 */
#define TOFP(x)         ((int32_t)((x) * 0x10000l))
#define TEXFP(x)        ((unsigned)((x) * 64u))

extern const struct game_room_bound         game_room1;
extern const struct game_room_bound         game_room2;
extern const struct game_room_bound         game_room3;

/*  5                                   #5 = -4.0, 6.0      #0 = -3.0, 6.0      #1 = -3.0, 4.0
 * /|\
 *  |      
 *  |    1--------------->2
 *  |--          |        |
 *  |    6<--9            |
 *  |   \|/ /|\         --|
 *  |    7-->8            |
 *  |          |         \|/
 *  4<--------------------3
 */

const struct game_2dvec_t           game_room1_vertices[] = {
    {   TOFP(  -3.00),  TOFP(   6.00)   },                          // 0        UNUSED
    {   TOFP(  -3.00),  TOFP(   4.00)   },                          // 1
    {   TOFP(   4.00),  TOFP(   4.00)   },                          // 2
    {   TOFP(   4.00),  TOFP(  -4.00)   },                          // 3
    {   TOFP(  -4.00),  TOFP(  -4.00)   },                          // 4
    {   TOFP(  -4.00),  TOFP(   6.00)   },                          // 5
    {   TOFP(  -3.00),  TOFP(   3.00)   },                          // 6
    {   TOFP(  -3.00),  TOFP(   2.00)   },                          // 7
    {   TOFP(  -2.00),  TOFP(   2.00)   },                          // 8
    {   TOFP(  -2.00),  TOFP(   3.00)   }                           // 9
};                                                                  //=10

const struct game_2dlineseg_t       game_room1_linesegs[] = {
    {                                                               // 0
        1,  2,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 1
        2,  3,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 2
        3,  4,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 3
        4,  5,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 1, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 4
        6,  7,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 5
        7,  8,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 6
        8,  9,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 7
        9,  6,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back)
    }
};                                                                  //=8

const struct game_2dsidedef_t       game_room1_sidedefs[] = {
    {                                                               // 0
        0,                                                          //  texture
        TEXFP(8)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 1
        0,                                                          //  texture
        TEXFP(10)                                                   //  texture width (-4 to 6)
    },
    {                                                               // 2
        0,                                                          //  texture
        TEXFP(2)                                                    //  texture width (4 to 6)
    },
    {                                                               // 3
        2,                                                          //  texture
        TEXFP(1)                                                    //  texture width (3 to 2)
    }
};                                                                  //=4

const struct game_room_bound*       game_room1_adj[] = {
    &game_room2,
    NULL
};

const struct game_room_bound        game_room1 = {
    {   TOFP(  -6.00),  TOFP(  -6.00)   },                          // tl (x,y)
    {   TOFP(   6.00),  TOFP(   6.00)   },                          // br (x,y)

    10,                                                             // vertex count
    game_room1_vertices,                                            // vertices

    8,                                                              // lineseg count
    game_room1_linesegs,                                            // linesegs

    4,                                                              // sidedef count
    game_room1_sidedefs,                                            // sidedefs

    game_room1_adj,                                                 // adjacent rooms

    0,                                                              // door count
    NULL,                                                           // doors

    0,                                                              // trigger count
    NULL                                                            // triggers
};

/*
 *  5
 * /|\
 *  |    0-\                            #1 = -3.0, 6.0     connects to #0 in room1
 *  4       --\                         #0 = -13.0, 16.0
 *  |          --\                      #2 = -4.0, 6.0     connects to #5 in room1
 *  |   5         --\                   #3 = -14.0, 6.0
 *  |    \           --\                #4 = -14.0, 16.0
 *  |     -\            1               #5 = -14.0, 17.0
 *  |       --6         |
 *  |                   |
 *  3<------------2     |
 *                     /|\
 *                    --|
 *                      |
 *                      8
 */

const struct game_2dvec_t           game_room2_vertices[] = {
    {   TOFP( -13.00),  TOFP(  16.00)   },                          // 0
    {   TOFP(  -3.00),  TOFP(   8.00)   },                          // 1
    {   TOFP(  -4.00),  TOFP(   6.00)   },                          // 2
    {   TOFP( -14.00),  TOFP(   6.00)   },                          // 3
    {   TOFP( -14.00),  TOFP(  16.00)   },                          // 4
    {   TOFP( -14.00),  TOFP(  17.00)   },                          // 5
    {   TOFP( -13.00),  TOFP(  14.00)   },                          // 6
    {   TOFP(  -7.00),  TOFP(   8.00)   },                          // 7
    {   TOFP(  -3.00),  TOFP(   4.00)   },                          // 8
};                                                                  //=9

const struct game_2dlineseg_t       game_room2_linesegs[] = {
    {                                                               // 0
        0,  1,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 1
        2,  3,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 2
        3,  4,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 3
        4,  5,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 2, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 4
        6,  7,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 1, 1 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 5
        1,  8,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    }
};                                                                  //=6

const struct game_2dsidedef_t       game_room2_sidedefs[] = {
    {                                                               // 0
        0,                                                          //  texture
        TEXFP(8)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 1
        1,                                                          //  texture
        TEXFP(6)                                                    //  texture width
    },
    {                                                               // 2
        1,                                                          //  texture
        TEXFP(1)                                                    //  texture width
    },
    {                                                               // 3
        0,                                                          //  texture
        TEXFP(3)                                                    //  texture width
    }
};                                                                  //=4

const struct game_room_bound*       game_room2_adj[] = {
    &game_room1,
    &game_room3,
    NULL
};

const struct game_room_bound        game_room2 = {
    {   TOFP( -16.00),  TOFP(   5.00)   },                          // tl (x,y)
    {   TOFP(  -3.00),  TOFP(  17.00)   },                          // br (x,y)

    9,                                                              // vertex count
    game_room2_vertices,                                            // vertices

    6,                                                              // lineseg count
    game_room2_linesegs,                                            // linesegs

    4,                                                              // sidedef count
    game_room2_sidedefs,                                            // sidedefs

    game_room2_adj,                                                 // adjacent rooms

    0,                                                              // door count
    NULL,                                                           // doors

    0,                                                              // trigger count
    NULL                                                            // triggers
};

/*
 * 15----16
 *  |    |         20----21
 *  |    |          |    |
 *  |1718|          |    |
 *  |    |          |    |
 * 14    0         19    |              #0  -13.0, 21.0
 * /    \|/      -/ 24   |              #1  -13.0, 20.0
 *13     1--->2-/     \  |              #2  -12.0, 20.0
 *|                    23|              #3  -12.0, 19.0
 *|      4<---3----------22             #4  -13.0, 19.0
 *|     \|/                             #5  -13.0, 18.0
 *12     5--->6--------------25         #6  -12.0, 18.0
 * \            |  |  |  |  | |         #7  -12.0, 17.0
 * 11    8<---7--------------26         #8  -13.0, 17.0
 *       |                              #9  -13.0, 16.0     connects to #0 in room2
 *      \|/                             #10 -14.0, 16.0     UNUSED
 *       9                              #11 -14.0, 17.0     connects to #4 in room2
 *                                      #12 -15.0, 18.0
 *                                      #13 -15.0, 20.0
 *                                      #14 -14.0, 21.0
 *                                      #15 -14.0, 25.0
 *                                      #16 -13.0, 25.0
 *                                      #17 -14.0, 22.0
 *                                      #18 -13.0, 22.0
 *                                      #19 -11.0, 22.0
 *                                      #20 -11.0, 25.0
 *                                      #21  -6.0, 25.0
 *                                      #22  -6.0, 19.0
 *                                      #23 -11.0, 22.0
 *                                      #24 -11.0, 19.0
 *                                      #25   0.0, 18.0
 *                                      #26   0.0, 17.0
 *                                      #27 -10.0, 18.0
 *                                      #28 -10.0, 17.0
 *                                      #29  -8.0, 18.0
 *                                      #30  -8.0, 17.0
 *                                      #31  -6.0, 18.0
 *                                      #32  -6.0, 17.0
 *                                      #33  -4.0, 18.0
 *                                      #34  -4.0, 17.0
 *                                      #35  -2.0, 18.0
 *                                      #36  -2.0, 17.0
 */

const struct game_2dvec_t           game_room3_vertices[] = {
    {   TOFP( -13.00),  TOFP(  21.00)   },                          // 0
    {   TOFP( -13.00),  TOFP(  20.00)   },                          // 1
    {   TOFP( -12.00),  TOFP(  20.00)   },                          // 2
    {   TOFP( -12.00),  TOFP(  19.00)   },                          // 3
    {   TOFP( -13.00),  TOFP(  19.00)   },                          // 4
    {   TOFP( -13.00),  TOFP(  18.00)   },                          // 5
    {   TOFP( -12.00),  TOFP(  18.00)   },                          // 6
    {   TOFP( -12.00),  TOFP(  17.00)   },                          // 7
    {   TOFP( -13.00),  TOFP(  17.00)   },                          // 8
    {   TOFP( -13.00),  TOFP(  16.00)   },                          // 9
    {   TOFP( -14.00),  TOFP(  16.00)   },                          // 10
    {   TOFP( -14.00),  TOFP(  17.00)   },                          // 11
    {   TOFP( -15.00),  TOFP(  18.00)   },                          // 12
    {   TOFP( -15.00),  TOFP(  20.00)   },                          // 13
    {   TOFP( -14.00),  TOFP(  21.00)   },                          // 14
    {   TOFP( -14.00),  TOFP(  25.00)   },                          // 15
    {   TOFP( -13.00),  TOFP(  25.00)   },                          // 16
    {   TOFP( -14.00),  TOFP(  22.00)   },                          // 17
    {   TOFP( -13.00),  TOFP(  22.00)   },                          // 18
    {   TOFP( -11.00),  TOFP(  22.00)   },                          // 19
    {   TOFP( -11.00),  TOFP(  25.00)   },                          // 20
    {   TOFP(  -6.00),  TOFP(  25.00)   },                          // 21
    {   TOFP(  -6.00),  TOFP(  19.00)   },                          // 22
    {   TOFP( -11.00),  TOFP(  22.00)   },                          // 23
    {   TOFP( -11.00),  TOFP(  19.00)   },                          // 24
    {   TOFP(   0.00),  TOFP(  18.00)   },                          // 25
    {   TOFP(   0.00),  TOFP(  17.00)   },                          // 26
    {   TOFP( -10.00),  TOFP(  18.00)   },                          // 27
    {   TOFP( -10.00),  TOFP(  17.00)   },                          // 28
    {   TOFP(  -8.00),  TOFP(  18.00)   },                          // 29
    {   TOFP(  -8.00),  TOFP(  17.00)   },                          // 30
    {   TOFP(  -6.00),  TOFP(  18.00)   },                          // 31
    {   TOFP(  -6.00),  TOFP(  17.00)   },                          // 32
    {   TOFP(  -4.00),  TOFP(  18.00)   },                          // 33
    {   TOFP(  -4.00),  TOFP(  17.00)   },                          // 34
    {   TOFP(  -2.00),  TOFP(  18.00)   },                          // 35
    {   TOFP(  -2.00),  TOFP(  17.00)   }                           // 36
};                                                                  //=37

const struct game_2dlineseg_t       game_room3_linesegs[] = {
    // 0->1->2
    {                                                               // 0
        0,  1,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 1
        1,  2,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    // 3->4->5->6
    {                                                               // 2
        3,  4,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back)
    },
    {                                                               // 3
        4,  5,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 4
        5,  6,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    // 7->8->9
    {                                                               // 5
        7,  8,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 6
        8,  9,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    // 11->12->13->14
    {                                                               // 7
        11,12,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 8
        12,13,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 1, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 9
        13,14,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 10
        14,15,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 2, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 11
        15,16,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 3, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 12
        16,0,                                                       //  vertices (start,end)
        0,                                                          //  flags
        { 2, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 13
        17,18,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 14
        2, 19,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 15
        19,20,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 16
        20,21,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 17
        21,22,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 18
        22, 3,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 19
        23,24,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 1, 1 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 20
        6, 25,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 5, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 21
        25,26,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 0, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 22
        26, 7,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 5, (~0u) }                                                //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 23
        27,28,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 24
        29,30,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 25
        31,32,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 26
        33,34,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    },
    {                                                               // 27
        35,36,                                                      //  vertices (start,end)
        0,                                                          //  flags
        { 4, 4 }                                                    //  sidedef (front, back) i.e. double-sided
    }
};                                                                  //=28

const struct game_2dsidedef_t       game_room3_sidedefs[] = {
    {                                                               // 0
        1,                                                          //  texture
        TEXFP(1)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 1
        1,                                                          //  texture
        TEXFP(2)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 2
        3,                                                          //  texture
        TEXFP(4)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 3
        3,                                                          //  texture
        TEXFP(1)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 4
        2,                                                          //  texture
        TEXFP(1)                                                    //  texture width (-4 to 4)
    },
    {                                                               // 5
        1,                                                          //  texture
        TEXFP(12)                                                   //  texture width
    }
};                                                                  //=6

const struct game_room_bound*       game_room3_adj[] = {
    &game_room2,
    NULL
};

const struct game_door_t            game_room3_doors[] = {
    {                                                               // 0
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        13/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 1
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        19/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 2
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        23/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 3
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        24/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 4
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        25/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 5
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        26/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    },
    {                                                               // 6
        0x0000/*open*/,
        0x0000/*open speed (change per 120Hz tick)*/,
        27/*lineseg*/,
        0/*origrot vertex (modified on load)*/
    }
};                                                                  //=7

const struct game_trigger_t         game_room3_triggers[] = {
    {                                                               // 0
        {   TOFP( -15.00),  TOFP(  20.00)   },                      // tl (x,y)
        {   TOFP( -12.00),  TOFP(  26.00)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        0                                                           // door
    },
    {                                                               // 1
        {   TOFP( -12.25),  TOFP(  18.50)   },                      // tl (x,y)
        {   TOFP(  -7.00),  TOFP(  26.00)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        1                                                           // door
    },
    {                                                               // 2
        {   TOFP( -11.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(  -7.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        2                                                           // door
    },
    {                                                               // 3
        {   TOFP(  -9.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(  -5.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        3                                                           // door
    },
    {                                                               // 4
        {   TOFP(  -7.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(  -3.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        4                                                           // door
    },
    {                                                               // 5
        {   TOFP(  -5.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(  -1.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        5                                                           // door
    },
    {                                                               // 6
        {   TOFP(  -3.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(   1.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_DOOR,                                                   // type
        0,                                                          // flags
        6                                                           // door
    },
    {                                                               // 7
        {   TOFP( -15.00),  TOFP(  20.00)   },                      // tl (x,y)
        {   TOFP( -12.00),  TOFP(  26.00)   },                      // br (x,y)
        GTT_TEXT,                                                   // type
        0,                                                          // flags
        0,                                                          // door
        "Minigame #1\n\nEnter the room to\nstart game"              // message
    },
    {                                                               // 8
        {   TOFP( -12.25),  TOFP(  18.50)   },                      // tl (x,y)
        {   TOFP(  -5.50),  TOFP(  26.00)   },                      // br (x,y)
        GTT_TEXT,                                                   // type
        0,                                                          // flags
        0,                                                          // door
        "Minigame #2\n\nGo to the back\nof the room to\nstart game"
    },
    {                                                               // 9
        {   TOFP( -12.00),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(   1.00),  TOFP(  18.50)   },                      // br (x,y)
        GTT_TEXT,                                                   // type
        0,                                                          // flags
        0,                                                          // door
        "Minigame #3\n\nWalk through all doors\nto the end of the hall\nto start game"
    },
    {                                                               // 10
        {   TOFP( -15.00),  TOFP(  24.50)   },                      // tl (x,y)
        {   TOFP( -12.00),  TOFP(  25.50)   },                      // br (x,y)
        GTT_MINIGAME,                                               // type
        0,                                                          // flags
        1                                                           // door (game)
    },
    {                                                               // 11
        {   TOFP( -11.50),  TOFP(  24.50)   },                      // tl (x,y)
        {   TOFP(  -5.50),  TOFP(  25.50)   },                      // br (x,y)
        GTT_MINIGAME,                                               // type
        0,                                                          // flags
        2                                                           // door (game)
    },
    {                                                               // 12
        {   TOFP(  -0.50),  TOFP(  16.50)   },                      // tl (x,y)
        {   TOFP(   0.50),  TOFP(  18.50)   },                      // br (x,y)
        GTT_MINIGAME,                                               // type
        0,                                                          // flags
        3                                                           // door (game)
    }
};                                                                  //=13

const struct game_room_bound        game_room3 = {
    {   TOFP( -16.00),  TOFP(  15.00)   },                          // tl (x,y)
    {   TOFP(   3.00),  TOFP(  26.00)   },                          // br (x,y)

    37,                                                             // vertex count
    game_room3_vertices,                                            // vertices

    28,                                                             // lineseg count
    game_room3_linesegs,                                            // linesegs

    6,                                                              // sidedef count
    game_room3_sidedefs,                                            // sidedefs

    game_room3_adj,                                                 // adjacent rooms

    7,                                                              // door count
    game_room3_doors,                                               // doors

    13,                                                             // trigger count
    game_room3_triggers                                             // triggers
};

const struct game_room_bound*       game_rooms[] = {
    &game_room1,
    &game_room2,
    &game_room3,
    NULL
};

void game_text_char_clear(void) {
    game_text_char_max = 0;
}

void game_text_char_add(const uint16_t x,const uint16_t y,const char *str) {
    uint16_t dx = x,dy = y;

    while (*str != 0) {
        const uint32_t c = utf8decode(&str);
        if (c == 0) break;
        if (c == '\n') {
            dy += 16;
            dx = x;
        }
        else {
            const unsigned int cdef = font_bmp_unicode_to_chardef(arial_large,c);
            if (cdef < arial_large->chardef_count) {
                if (game_text_char_max >= GAME_TEXT_CHAR_MAX)
                    fatal("game_text_char_add too many char");

                game_text_char[game_text_char_max].c = cdef;
                game_text_char[game_text_char_max].x = dx;
                game_text_char[game_text_char_max].y = dy;
                game_text_char_max++;

                dx += arial_large->chardef[cdef].xadvance;
            }
        }
    }
}

void game_clear_level(void) {
    game_vertex_max = 0;
    game_lineseg_max = 0;
    game_sidedef_max = 0;
    game_trigger_max = 0;
    game_text_char_max = 0;
    game_door_max = 0;
}

void game_load_room(const struct game_room_bound *room) {
    const unsigned base_vertex=game_vertex_max,base_lineseg=game_lineseg_max,base_sidedef=game_sidedef_max;
    const unsigned base_trigger=game_trigger_max,base_door=game_door_max;

    (void)base_trigger;

    if ((game_vertex_max+room->vertex_count) > GAME_VERTICES)
        fatal("game_load_room too many vertices");
    if ((game_lineseg_max+room->lineseg_count) > GAME_LINESEG)
        fatal("game_load_room too many lineseg");
    if ((game_sidedef_max+room->sidedef_count) > GAME_SIDEDEFS)
        fatal("game_load_room too many sidedef");
    if ((game_door_max+room->door_count) > GAME_DOORS)
        fatal("game_load_room too many doors");
    if ((game_trigger_max+room->trigger_count) > GAME_TRIGGERS)
        fatal("game_load_room too many triggers");

    {
        unsigned i;
        const struct game_2dvec_t *s = room->vertex;
        struct game_2dvec_t *d = game_vertex+game_vertex_max;

        for (i=0;i < room->vertex_count;i++,d++,s++)
            *d = *s;

        game_vertex_max += room->vertex_count;
    }

    {
        unsigned i;
        const struct game_2dlineseg_t *s = room->lineseg;
        struct game_2dlineseg_t *d = game_lineseg+game_lineseg_max;

        for (i=0;i < room->lineseg_count;i++,d++,s++) {
            d->sidedef[0] = (s->sidedef[0] != (~0u)) ? (s->sidedef[0] + base_sidedef) : (~0u);
            d->sidedef[1] = (s->sidedef[1] != (~0u)) ? (s->sidedef[1] + base_sidedef) : (~0u);
            d->start = s->start + base_vertex;
            d->end = s->end + base_vertex;
            d->flags = s->flags;
        }

        game_lineseg_max += room->lineseg_count;
    }

    {
        unsigned i;
        const struct game_2dsidedef_t *s = room->sidedef;
        struct game_2dsidedef_t *d = game_sidedef+game_sidedef_max;

        for (i=0;i < room->sidedef_count;i++,d++,s++)
            *d = *s;

        game_sidedef_max += room->sidedef_count;
    }

    {
        unsigned i;
        const struct game_door_t *s = room->door;
        struct game_door_t *d = game_door+game_door_max;

        for (i=0;i < room->door_count;i++,d++,s++) {
            *d = *s;
            d->origrot_vertex = (~0u);
            d->door_lineseg += base_lineseg;
            if (d->door_lineseg < game_lineseg_max) {
                if (game_vertex_max < GAME_VERTICES) {
                    const unsigned cvertex = game_vertex_max++;
                    game_vertex[cvertex] = game_vertex[game_lineseg[d->door_lineseg].end];
                    d->origrot_vertex = cvertex;
                }
            }
        }

        game_door_max += room->door_count;
    }

    {
        unsigned i;
        const struct game_trigger_t *s = room->trigger;
        struct game_trigger_t *d = game_trigger+game_trigger_max;

        for (i=0;i < room->trigger_count;i++,d++,s++) {
            *d = *s;
            if (d->type == GTT_DOOR) {
                d->door += base_door;
            }
        }

        game_trigger_max += room->trigger_count;
    }

}

unsigned point_in_room(const struct game_2dvec_t *pt,const struct game_room_bound *room) {
    if (pt->x >= room->tl.x && pt->x <= room->br.x) {
        if (pt->y >= room->tl.y && pt->y <= room->br.y) {
            return 1;
        }
    }

    return 0;
}

const struct game_room_bound* game_cur_room;

void game_load_room_and_adj(const struct game_room_bound* room) {
    game_load_room(room);

    {
        const struct game_room_bound** also = room->also;
        if (also != NULL) {
            while (*also != NULL) {
                game_load_room(*also);
                also++;
            }
        }
    }
}

void game_load_room_from_pos(void) {
    const struct game_room_bound **rooms = game_rooms;

    while (*rooms != NULL) {
        if (point_in_room(&game_position,*rooms)) {
            game_clear_level();
            game_load_room_and_adj(*rooms);
            game_cur_room = *rooms;
            break;
        }

        rooms++;
    }
}

void game_reload_if_needed_on_pos(const struct game_2dvec_t *pos) {
    if (game_cur_room != NULL) {
        if (point_in_room(pos,game_cur_room))
            return;
    }

    game_load_room_from_pos();
}

/* These must differ slightly or else you get "stuck" walking into corners */
#define wall_clipxwidth TOFP(0.41)
#define wall_clipywidth TOFP(0.42)

void game_player_move(const int32_t dx,const int32_t dy) {
    const int32_t ox = game_position.x;
    const int32_t oy = game_position.y;
    unsigned int ls;

    if (dx == (int32_t)0 && dy == (int32_t)0)
        return;

    game_position.x += dx;
    game_position.y += dy;

    if (noclip_on())
        return;

    for (ls=0;ls < game_lineseg_max;ls++) {
        const struct game_2dlineseg_t *lseg = &game_lineseg[ls];
        const struct game_2dvec_t *v1 = &game_vertex[lseg->start];
        const struct game_2dvec_t *v2 = &game_vertex[lseg->end];
        const int32_t ldx = v2->x - v1->x;
        const int32_t ldy = v2->y - v1->y;
        int32_t minx,miny,maxx,maxy;

        if (v1->x <= v2->x) {
            minx = v1->x; maxx = v2->x;
        }
        else {
            minx = v2->x; maxx = v1->x;
        }

        if (v1->y <= v2->y) {
            miny = v1->y; maxy = v2->y;
        }
        else {
            miny = v2->y; maxy = v1->y;
        }

        if (ldx != 0l && game_position.x >= (minx - wall_clipxwidth) && game_position.x <= (maxx + wall_clipxwidth)) {
            /* y = mx + b          m = ldy/ldx    b = v1->y */
            int32_t ly = v1->y + (int32_t)(((int64_t)ldy * (int64_t)(game_position.x - v1->x)) / (int64_t)ldx);
            unsigned side = ldx < 0l ? 1 : 0;

            if (ly > maxy)
                ly = maxy;
            if (ly < miny)
                ly = miny;

            if (oy < ly) {
                if (game_position.y > (ly - wall_clipywidth) && lseg->sidedef[side] != (~0u) && dy > 0l)
                    game_position.y = (ly - wall_clipywidth);
            }
            else {
                if (game_position.y < (ly + wall_clipywidth) && lseg->sidedef[side^1] != (~0u) && dy < 0l)
                    game_position.y = (ly + wall_clipywidth);
            }
        }
        if (ldy != 0l && game_position.y >= (miny - wall_clipxwidth) && game_position.y <= (maxy + wall_clipxwidth)) {
            /* x = my + b          m = ldx/ldy    b = v1->x */
            int32_t lx = v1->x + (int32_t)(((int64_t)ldx * (int64_t)(game_position.y - v1->y)) / (int64_t)ldy);
            unsigned side = ldy >= 0l ? 1 : 0;

            if (lx > maxx)
                lx = maxx;
            if (lx < minx)
                lx = minx;

            if (ox < lx) {
                if (game_position.x > (lx - wall_clipywidth) && lseg->sidedef[side] != (~0u) && dx > 0l)
                    game_position.x = (lx - wall_clipywidth);
            }
            else {
                if (game_position.x < (lx + wall_clipywidth) && lseg->sidedef[side^1] != (~0u) && dx < 0l)
                    game_position.x = (lx + wall_clipywidth);
            }
        }
    }
}

void game_exe_init(void) {
    memset(game_texture,0,sizeof(game_texture));
    game_vslice = NULL;
}

void game_vslice_free(void) {
    if (game_vslice != NULL) {
        free(game_vslice);
        game_vslice = NULL;
    }
}

void game_vslice_init(void) {
    game_vslice = malloc(GAME_VSLICE_MAX * sizeof(struct game_vslice_t));
    if (game_vslice == NULL) fatal("game_vslice alloc");
}

static void game_door_reposition(const unsigned i) {
    if (game_door[i].origrot_vertex < game_vertex_max) {
        const struct game_2dvec_t orig = game_vertex[game_door[i].origrot_vertex];
        const struct game_2dvec_t pivot = game_vertex[game_lineseg[game_door[i].door_lineseg].start];
        struct game_2dvec_t *mod = &game_vertex[game_lineseg[game_door[i].door_lineseg].end];
        unsigned ga = game_door[i].open / 33u; /* 0x10000 / 32 = 2048   2048 = 90 degrees  but go a bit more so the door doesn't quite go into the wall */
        int32_t dx = orig.x - pivot.x;
        int32_t dy = orig.y - pivot.y;
        int32_t ndx = (int32_t)((((int64_t)dx * (int64_t)cos2048fps16_lookup(ga)) - ((int64_t)dy * (int64_t)sin2048fps16_lookup(ga))) >> 15l);
        int32_t ndy = (int32_t)((((int64_t)dy * (int64_t)cos2048fps16_lookup(ga)) + ((int64_t)dx * (int64_t)sin2048fps16_lookup(ga))) >> 15l);

        mod->x = pivot.x + ndx;
        mod->y = pivot.y + ndy;
    }
}

static void game_door_anim(const unsigned i,const uint32_t deltat) {
    {
        const int32_t c = (int32_t)game_door[i].open + (int32_t)game_door[i].open_speed * (int32_t)deltat;
        if (c >= 0xFFFFl && game_door[i].open_speed > 0) {
            game_door[i].open = 0xFFFFu;
            game_door[i].open_speed = 0;
        }
        else if (c <= 0x0000l && game_door[i].open_speed < 0) {
            game_door[i].open = 0x0000u;
            game_door[i].open_speed = 0;
        }
        else {
            game_door[i].open = (uint16_t)c;
        }
    }

    game_door_reposition(i);
}

static void game_trigger_act(const unsigned i,const unsigned on) {
    if (game_trigger[i].type == GTT_DOOR) {
        if (game_trigger[i].door < game_door_max) {
            game_door[game_trigger[i].door].open_speed = on ? 0x400 : -0x400;
        }
    }
    else if (game_trigger[i].type == GTT_TEXT) {
        game_text_char_clear();
        if (on && game_trigger[i].msg != NULL)
            game_text_char_add(50,50,game_trigger[i].msg);
    }
    else if (game_trigger[i].type == GTT_MINIGAME) {
        game_minigame_select = (uint8_t)game_trigger[i].door;
    }
}

static void game_trigger_check(void) {
    unsigned i;

    for (i=0;i < game_trigger_max;i++) {
        if (game_position.x >= game_trigger[i].tl.x && game_position.x <= game_trigger[i].br.x &&
            game_position.y >= game_trigger[i].tl.y && game_position.y <= game_trigger[i].br.y) {
            if (!(game_trigger[i].flags & GTF_TRIGGER_ON)) {
                game_trigger[i].flags |= GTF_TRIGGER_ON;
                game_trigger_act(i,1);
            }
        }
        else {
            if (game_trigger[i].flags & GTF_TRIGGER_ON) {
                game_trigger[i].flags &= ~GTF_TRIGGER_ON;
                game_trigger_act(i,0);
            }
        }
    }
}

/* 0 = normal
 * 1 = return from minigame */
void game_loop(const unsigned int game_mode) {
#define MAX_VSLICE_DRAW     8
    unsigned int vslice_draw_count;
    uint16_t vslice_draw[MAX_VSLICE_DRAW];
    struct game_vslice_t *vsl;
    uint32_t prev,cur;
    unsigned int x2;
    unsigned int o;
    unsigned int i;
    unsigned int x;

    /* seqanim rotozoomer needs sin2048 */
    if (sin2048fps16_open())
        fatal("cannot open sin2048");

    if (font_bmp_do_load_arial_large())
        fatal("arial");

    game_vslice_init();
    game_texture_load(0,"watx0001.png",GAMETEX_LOAD_PAL0);
    game_texture_load(1,"watx0002.png",0);
    game_texture_load(2,"watx0003.png",0);
    game_texture_load(3,"watx0004.png",0);

    vga_palette_lseek(GAME_PAL_TEXT);
    vga_palette_write(4,4,31);

    game_flags = 0;
    game_vertex_max = 0;
    game_lineseg_max = 0;
    game_sidedef_max = 0;
    game_door_max = 0;

    /* init pos */
    game_cur_room = NULL;
    game_current_room = -1;
    game_position.x = 0;
    game_position.y = 0;
    game_angle = 0; /* looking straight ahead */

    /* if coming back from minigame, put the player outside the doors */
    if (game_mode == 1u) {
        game_position.x = TOFP(-13.50);
        game_position.y = TOFP( 15.00);
    }

    game_minigame_select = 0xFFu;

    game_load_room_from_pos();

    init_keyboard_irq();

    cur = read_timer_counter();

    while (1) {
        prev = cur;
        cur = read_timer_counter();

        if (game_minigame_select != 0xFFu) break;

        if (kbdown_test(KBDS_ESCAPE)) break;

        if (kbdown_test(KBDS_LSHIFT) || kbdown_test(KBDS_RSHIFT)) {
            if (kbdown_test(KBDS_UP_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move( (((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 15l), (((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 15l) );
                game_reload_if_needed_on_pos(&game_position);
            }
            if (kbdown_test(KBDS_DOWN_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move(-(((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l),-(((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l) );
                game_reload_if_needed_on_pos(&game_position);
            }
        }
        else {
            if (kbdown_test(KBDS_UP_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move( (((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l), (((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l) );
                game_reload_if_needed_on_pos(&game_position);
            }
            if (kbdown_test(KBDS_DOWN_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move(-(((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 60l),-(((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 60l) );
                game_reload_if_needed_on_pos(&game_position);
            }
        }
        if (kbdown_test(KBDS_LCTRL) || kbdown_test(KBDS_RCTRL)) {
            if (kbdown_test(KBDS_LEFT_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move(-(((int32_t)sin2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l),-(((int32_t)cos2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l) );
                game_reload_if_needed_on_pos(&game_position);
            }
            if (kbdown_test(KBDS_RIGHT_ARROW)) {
                const unsigned ga = game_angle >> 3u;
                game_player_move( (((int32_t)sin2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l), (((int32_t)cos2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l) );
                game_reload_if_needed_on_pos(&game_position);
            }
        }
        else {
            if (kbdown_test(KBDS_LEFT_ARROW))
                game_angle -= (((int32_t)(cur - prev)) << 13l) / 60l;
            if (kbdown_test(KBDS_RIGHT_ARROW))
                game_angle += (((int32_t)(cur - prev)) << 13l) / 60l;
        }

        {
            int c = kbd_buf_read();
            if (c == 0x2E/*F10*/ && kbdown_test(KBDS_LSHIFT)) {
                if (kbdown_test(KBDS_LCTRL))
                    game_flags |= GF_CHEAT_NOCLIP;
                else
                    game_flags &= ~GF_CHEAT_NOCLIP;
            }
        }

        /* clear screen */
        vga_write_sequencer(0x02/*map mask*/,0xF);
        vga_rep_stosw(vga_state.vga_graphics_ram,0,((320u/4u)*200)/2u);

        /* project and render */
        game_vslice_alloc = 0;
        for (i=0;i < GAME_VSLICE_DRAW;i++) game_vslice_draw[i] = ~0u;

        /* triggers */
        game_trigger_check();

        /* door management */
        for (i=0;i < game_door_max;i++) {
            if (game_door[i].open_speed != 0)
                game_door_anim(i,cur - prev);
        }

        for (i=0;i < game_vertex_max;i++) {
            /* TODO: 2D rotation based on player angle */
            /* TODO: Perhaps only the line segments we draw */
            game_vertexrot[i].x = game_vertex[i].x - game_position.x;
            game_vertexrot[i].y = game_vertex[i].y - game_position.y;

            {
                const unsigned ga = game_angle >> 3u;
                const int64_t inx = ((int64_t)game_vertexrot[i].x * (int64_t)cos2048fps16_lookup(ga)) - ((int64_t)game_vertexrot[i].y * (int64_t)sin2048fps16_lookup(ga));
                const int64_t iny = ((int64_t)game_vertexrot[i].y * (int64_t)cos2048fps16_lookup(ga)) + ((int64_t)game_vertexrot[i].x * (int64_t)sin2048fps16_lookup(ga));
                game_vertexrot[i].x = (int32_t)(inx >> 15ll);
                game_vertexrot[i].y = (int32_t)(iny >> 15ll);
            }
        }

        for (i=0;i < game_lineseg_max;i++)
            game_project_lineseg(i);

        for (i=0;i < GAME_VSLICE_DRAW;i++) {
            uint16_t vslice,vi;

            vslice_draw_count = 0;
            vslice = game_vslice_draw[i];
            while (vslice != (~0u) && vslice_draw_count < MAX_VSLICE_DRAW) {
                vslice_draw[vslice_draw_count++] = vslice;
                vslice = game_vslice[vslice].next;
            }

            vga_write_sequencer(0x02/*map mask*/,1u << (i & 3u));

            for (vi=vslice_draw_count;vi != 0;) {
                __segment vs = FP_SEG(vga_state.vga_graphics_ram);
                __segment texs;
                unsigned texo;
                uint16_t tf,ts,tw;

                vslice = vslice_draw[--vi];
                vsl = &game_vslice[vslice];
                x2 = (unsigned int)((vsl->floor) < 0 ? 0 : vsl->floor);
                x = (unsigned int)((vsl->ceil) < 0 ? 0 : vsl->ceil);
                if (x2 > 200) x2 = 200;
                if (x > 200) x = 200;
                if (x >= x2) continue;

                texs = FP_SEG(game_texture[vsl->tex_n].tex);
                texo = FP_OFF(game_texture[vsl->tex_n].tex) + (vsl->tex_x * 64u);
                {
                    const uint32_t s = (0x10000ul * 64ul) / (uint32_t)(vsl->floor - vsl->ceil);
                    tw = (uint16_t)(s >> 16ul);
                    ts = (uint16_t)(s & 0xFFFFul);
                    tf = 0;
                }

                o = (i >> 2u) + (x * 80u) + FP_OFF(vga_state.vga_graphics_ram);
                x2 -= x;

                if (vsl->ceil < 0) {
                    uint32_t adv = ((uint32_t)(-vsl->ceil) * (uint32_t)(ts)) + (uint32_t)(tf);
                    texo += (uint16_t)(-vsl->ceil) * (uint16_t)(tw);
                    texo += (uint16_t)(adv >> 16ul);
                    tf = (uint16_t)(adv);
                }

                /* do not access data local variables between PUSH DS and POP DS.
                 * local stack allocated variables are fine until the PUSH BP because most compilers
                 * including Open Watcom code locals access as some form of MOV ...,[BP+n] */
                __asm {
                    mov         cx,x2
                    mov         si,texo
                    mov         di,o
                    mov         dx,tw
                    mov         ax,tf
                    mov         bx,ts
                    push        ds
                    mov         es,vs
                    mov         ds,texs
                    push        bp
                    mov         bp,bx
yal1:               ; CX = x2  DS:SI = texs:texo  ES:DI = vs:o  DX = tw  AX = tf  BP = ts  BX = (left aside for pixel copy)
                    mov         bl,[si]
                    mov         es:[di],bl
                    add         di,80               ; o += 80
                    add         ax,bp               ; ts += tf
                    adc         si,dx               ; texo += tw + CF
                    loop        yal1
                    pop         bp
                    pop         ds
                }

                vslice = vsl->next;
            }
        }

        /* text overlay */
        {
            unsigned int i;

            for (i=0;i < game_text_char_max;i++)
                font_bmp_draw_chardef_vga8u(arial_large,game_text_char[i].c,game_text_char[i].x,game_text_char[i].y,GAME_PAL_TEXT);
        }

        /* present to screen, flip pages, wait for vsync */
        vga_swap_pages(); /* current <-> next */
        vga_update_disp_cur_page();
        vga_wait_for_vsync(); /* wait for vsync */
    }

    /* fade out to minigame */
    if (game_minigame_select != 0xFFu) {
        unsigned int i,j;

        outp(0x3C7,0); // read from VGA palette index 0
        for (i=0;i < 768;i++) common_tmp_small[i] = inp(0x3C9);

        for (j=0;j < 16;j++) {
            vga_wait_for_vsync(); /* wait for vsync */
            outp(0x3C8,0); // write palette from 0
            for (i=0;i < 768;i++) outp(0x3C9,(common_tmp_small[i] * (16u-j))>>4u);
        }
    }

    /* wait for user to let go of arrow keys, etc. */
    while (kbdown_test(KBDS_UP_ARROW) || kbdown_test(KBDS_DOWN_ARROW) || kbdown_test(KBDS_LEFT_ARROW) || kbdown_test(KBDS_RIGHT_ARROW));

    restore_keyboard_irq();
    game_texture_freeall();
    game_vslice_free();
}

/*---------------------------------------------------------------------------*/
/* main                                                                      */
/*---------------------------------------------------------------------------*/

void gen_res_free(void) {
    seq_com_cleanup();
    sin2048fps16_free();
    font_bmp_free(&arial_small);
    font_bmp_free(&arial_medium);
    font_bmp_free(&arial_large);
    dumbpack_close(&sorc_pack);
}

int main(int argc,char **argv) {
    probe_dos();
	cpu_probe();
    if (cpu_basic_level < 3) {
        printf("This game requires a 386 or higher\n");
        return 1;
    }

	if (!probe_8254()) {
		printf("No 8254 detected.\n");
		return 1;
	}
	if (!probe_8259()) {
		printf("No 8259 detected.\n");
		return 1;
	}
    if (!probe_vga()) {
        printf("VGA probe failed.\n");
        return 1;
    }
    if (!(vga_state.vga_flags & VGA_IS_VGA)) {
        printf("This game requires VGA\n");
        return 1;
    }
    detect_keyboard();

#if TARGET_MSDOS == 16
# if 0 // not using it yet
    probe_emm();            // expanded memory support
    probe_himem_sys();      // extended memory support
# endif
#endif

    if (argc > 1 && !strcmp(argv[1],"KBTEST")) {
        printf("Keyboard test. Hit keys to see scan codes. ESC to exit.\n");

        init_keyboard_irq();
        while (1) {
            int k = kbd_buf_read();
            if (k >= 0) printf("0x%x\n",k);
            if (k == KBDS_ESCAPE) break;
        }

        printf("Dropping back to DOS.\n");
        unhook_irqs(); // including keyboard
        return 0;
    }

    game_exe_init();
    seq_com_exe_init();

    init_timer_irq();
    init_vga256unchained();

    if (argc > 1 && !strcmp(argv[1],"MGRET")) { /* minigame return */
        game_loop(1);
    }
    else {
        seq_intro();
        game_loop(0);
    }

    gen_res_free();
    check_heap();
    unhook_irqs();
    restore_text_mode();

    //debug
    dbg_heap_list();

    if (game_minigame_select != 0xFFu)
        return 0x40+game_minigame_select;

    return 0;
}

