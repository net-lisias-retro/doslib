
#include <stdio.h>
#include <assert.h>
#include <endian.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <png.h>    /* libpng */
#include <zlib.h>   /* zlib */

static char*            in_fnt = NULL;
static char*            in_png = NULL;
static char*            out_png = NULL;

static png_color        gen_png_pal[256] = {0};
static int              gen_png_pal_count = 0;

static unsigned char*   gen_png_image = NULL;
static png_bytep*       gen_png_image_rows = NULL;
static png_uint_32      gen_png_width = 0,gen_png_height = 0;
static int              gen_png_bit_depth = 0;
static int              gen_png_color_type = 0;
static int              gen_png_interlace_method = 0;
static int              gen_png_compression_method = 0;
static int              gen_png_filter_method = 0;
static png_byte         gen_png_trns[256];
static int              gen_png_trns_count = 0;

static char             temp[16384];
static char             temp2[16384];

#define MAX_CHARS       512
#define MAX_KERNS       512

struct chardef {
/* char id=32   x=178   y=65    width=3     height=1     xoffset=-1    yoffset=15    xadvance=4     page=0  chnl=15 */
    uint16_t        id;             /* unicode code value */
    uint8_t         x,y;            /* starting x and y (upper left corner). These images are 256x256 so it's fine to use uint8_t */
    uint8_t         w,h;            /* width and height at x,y */
    int8_t          xoffset,yoffset;/* offset from draw position to draw */
    int8_t          xadvance;       /* how many to advance */
    /* ignored: page */
    /* ignored: chnl */
};

struct kerndef {
/* kerning first=32  second=65  amount=-1 */
    uint16_t        first;
    uint16_t        second;
    int8_t          amount;
};

struct chardef      chardefs[MAX_CHARS];
unsigned int        chardef_count = 0;

struct kerndef      kerndefs[MAX_KERNS];
unsigned int        kerndef_count = 0;

unsigned int        img_used_w=0,img_used_h=0;

int read_fnt(void) {
    FILE *fp;

    if (chardef_count != 0 || kerndef_count != 0)
        return -1;

    if ((fp=fopen(in_fnt,"r")) == NULL)
        return -1;

    while (!feof(fp) && !ferror(fp)) {
        if (fgets(temp,sizeof(temp),fp) == NULL) break;

        if (!strncmp(temp,"char ",5)) {
            if (chardef_count < MAX_CHARS) {
                struct chardef *cdef = &chardefs[chardef_count++];
                char *s = temp+5;
                char *name;

                while (*s != 0 && *s != '\n') {
                    /* char id=32   x=178   y=65    width=3     height=1     xoffset=-1    yoffset=15    xadvance=4     page=0  chnl=15 */
                    while (*s == ' ' || *s == '\t') s++;
                    if (*s == 0 || *s == '\n') break;

                    name = s;
                    while (*s != 0 && *s != '=') s++;
                    if (*s == '=') *s++ = 0; /* ASCIIZ snip */

                    if (!strcmp(name,"id"))
                        cdef->id = strtoul(s,&s,10);
                    else if (!strcmp(name,"x"))
                        cdef->x = strtoul(s,&s,10);
                    else if (!strcmp(name,"y"))
                        cdef->y = strtoul(s,&s,10);
                    else if (!strcmp(name,"width"))
                        cdef->w = strtoul(s,&s,10);
                    else if (!strcmp(name,"height"))
                        cdef->h = strtoul(s,&s,10);
                    else if (!strcmp(name,"xoffset"))
                        cdef->xoffset = strtol(s,&s,10);
                    else if (!strcmp(name,"yoffset"))
                        cdef->yoffset = strtol(s,&s,10);
                    else if (!strcmp(name,"xadvance"))
                        cdef->xadvance = strtol(s,&s,10);

                    while (*s != 0 && *s != ' ') s++;
                }
            }
        }
        else if (!strncmp(temp,"kerning ",8)) {
            if (chardef_count < MAX_CHARS) {
                struct kerndef *kdef = &kerndefs[kerndef_count++];
                char *s = temp+8;
                char *name;

                while (*s != 0 && *s != '\n') {
                    /* kerning first=32  second=65  amount=-1 */
                    while (*s == ' ' || *s == '\t') s++;
                    if (*s == 0 || *s == '\n') break;

                    name = s;
                    while (*s != 0 && *s != '=') s++;
                    if (*s == '=') *s++ = 0; /* ASCIIZ snip */

                    if (!strcmp(name,"first"))
                        kdef->first = strtoul(s,&s,10);
                    else if (!strcmp(name,"second"))
                        kdef->second = strtoul(s,&s,10);
                    else if (!strcmp(name,"amount"))
                        kdef->amount = strtol(s,&s,10);

                    while (*s != 0 && *s != ' ') s++;
                }
            }
        }
    }

    {
        unsigned int i;

        img_used_w = img_used_h = 0;
        for (i=0;i < chardef_count;i++) {
            struct chardef *cdef = &chardefs[i];

            if (img_used_w < (cdef->x+cdef->w))
                img_used_w = (cdef->x+cdef->w);
            if (img_used_h < (cdef->y+cdef->h))
                img_used_h = (cdef->y+cdef->h);
        }
    }

    printf("font: %u chardefs, %u kerndefs\n",chardef_count,kerndef_count);
    printf("uses: %u x %u\n",img_used_w,img_used_h);

    /* 1bpp PNGs encode 8 bits per pixel whether or not you use all of them */
    if (gen_png_bit_depth == 1)
        img_used_w = (img_used_w + 7u) & (~7u);

    printf("crop: %u x %u\n",img_used_w,img_used_h);

    fclose(fp);
    return 0;
}

static void free_gen_png(void) {
    if (gen_png_image) {
        free(gen_png_image);
        gen_png_image = NULL;
    }
    if (gen_png_image_rows) {
        free(gen_png_image_rows);
        gen_png_image_rows = NULL;
    }
}

static void help(void) {
    fprintf(stderr,"fontproc -i <input PNG> -o <output PNG> -f <FNT description>\n");
    fprintf(stderr,"Convert a paletted PNG to another paletted PNG,\n");
    fprintf(stderr,"rearranging the palette to match palette PNG.\n");
}

static int parse_argv(int argc,char **argv) {
    int i = 1;
    char *a;

    while (i < argc) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help();
                return 1;
            }
            else if (!strcmp(a,"i")) {
                if ((in_png = argv[i++]) == NULL)
                    return 1;
            }
            else if (!strcmp(a,"o")) {
                if ((out_png = argv[i++]) == NULL)
                    return 1;
            }
            else if (!strcmp(a,"f")) {
                if ((in_fnt = argv[i++]) == NULL)
                    return 1;
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unexpected arg %s\n",a);
            return 1;
        }
    }

    if (in_fnt == NULL) {
        fprintf(stderr,"Input -f fnt required\n");
        return 1;
    }

    if (in_png == NULL) {
        fprintf(stderr,"Input -i png required\n");
        return 1;
    }

    if (out_png == NULL) {
        fprintf(stderr,"Output -o png required\n");
        return 1;
    }

    return 0;
}

static int load_in_png(void) {
    png_structp png_context = NULL;
    png_infop png_context_info = NULL;
    png_infop png_context_end = NULL;
    png_uint_32 png_width = 0,png_height = 0;
    int png_bit_depth = 0;
    int png_color_type = 0;
    int png_interlace_method = 0;
    int png_compression_method = 0;
    int png_filter_method = 0;
    FILE *fp = NULL;
    int ret = 1;

    free_gen_png();

    if (in_png == NULL)
        return 1;

    fp = fopen(in_png,"rb");
    if (fp == NULL)
        return 1;

    png_context = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL/*error*/,NULL/*error fn*/,NULL/*warn fn*/);
    if (png_context == NULL) goto fail;

    png_context_info = png_create_info_struct(png_context);
    if (png_context_info == NULL) goto fail;

    png_init_io(png_context, fp);
    png_read_info(png_context, png_context_info);

    if (!png_get_IHDR(png_context, png_context_info, &png_width, &png_height, &png_bit_depth, &png_color_type, &png_interlace_method, &png_compression_method, &png_filter_method))
        goto fail;

    if (png_color_type != PNG_COLOR_TYPE_PALETTE) {
        fprintf(stderr,"Input PNG not paletted\n");
        goto fail;
    }

    {
        png_color* pal = NULL;
        int pal_count = 0;

        /* FIXME: libpng makes no reference to freeing this. Do you? */
        if (png_get_PLTE(png_context, png_context_info, &pal, &pal_count) == 0) {
            fprintf(stderr,"Unable to get Input PNG palette\n");
            goto fail;
        }

        /* I think libpng only points at it's in memory buffers. Copy it. */
        gen_png_pal_count = pal_count;
        if (pal_count != 0 && pal_count <= 256)
            memcpy(gen_png_pal,pal,sizeof(png_color) * pal_count);
    }

    /* we gotta preserve alpha transparency too */
    gen_png_trns_count = 0;
    {
        png_color_16p trans_values = 0; /* throwaway value */
        png_bytep trans = NULL;
        int trans_count = 0;

        gen_png_trns_count = 0;
        if (png_get_tRNS(png_context, png_context_info, &trans, &trans_count, &trans_values) != 0) {
            if (trans_count != 0 && trans_count <= 256) {
                memcpy(gen_png_trns, trans, trans_count);
                gen_png_trns_count = trans_count;
            }
        }
    }

    if (png_width <= 0 || png_width > 4096 || png_height <= 0 || png_height > 4096)
        goto fail;
    if (png_color_type != PNG_COLOR_TYPE_PALETTE)
        goto fail;

    gen_png_image = malloc((png_width * png_height) + 4096);
    if (gen_png_image == NULL)
        goto fail;

    gen_png_image_rows = (png_bytep*)malloc(sizeof(png_bytep) * png_height);
    if (gen_png_image_rows == NULL)
        goto fail;

    {
        unsigned int y;
        for (y=0;y < png_height;y++)
            gen_png_image_rows[y] = gen_png_image + (y * png_width);
    }

    png_read_rows(png_context, gen_png_image_rows, NULL, png_height);

    gen_png_width = png_width;
    gen_png_height = png_height;
    gen_png_bit_depth = png_bit_depth;
    gen_png_color_type = png_color_type;
    gen_png_interlace_method = png_interlace_method;
    gen_png_compression_method = png_compression_method;
    gen_png_filter_method = png_filter_method;

    /* success */
    ret = 0;
fail:
    if (png_context != NULL)
        png_destroy_read_struct(&png_context,&png_context_info,&png_context_end);

    if (ret)
        fprintf(stderr,"Failed to load input PNG\n");

    fclose(fp);
    return ret;
}

static int save_out_png(void) {
    png_structp png_context = NULL;
    png_infop png_context_info = NULL;
    png_infop png_context_end = NULL;
    FILE *fp = NULL;
    unsigned int i;
    int ret = 1;

    if (out_png == NULL)
        return 1;

    if (gen_png_width <= 0 || gen_png_width > 4096 || gen_png_height <= 0 || gen_png_height > 4096)
        return 1;
    if (gen_png_color_type != PNG_COLOR_TYPE_PALETTE)
        return 1;
    if (gen_png_image == NULL || gen_png_image_rows == NULL)
        return 1;

    fp = fopen(out_png,"wb");
    if (fp == NULL)
        return 1;

    png_context = png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL/*error*/,NULL/*error fn*/,NULL/*warn fn*/);
    if (png_context == NULL) goto fail;

    png_context_info = png_create_info_struct(png_context);
    if (png_context_info == NULL) goto fail;

    png_init_io(png_context, fp);

    png_set_IHDR(png_context, png_context_info, gen_png_width, gen_png_height, gen_png_bit_depth, gen_png_color_type, gen_png_interlace_method, gen_png_compression_method, gen_png_filter_method);

    png_set_PLTE(png_context, png_context_info, gen_png_pal, gen_png_pal_count);

    /* we gotta preserve alpha transparency too */
    if (gen_png_trns_count != 0) {
        png_set_tRNS(png_context, png_context_info, gen_png_trns, gen_png_trns_count, 0);
    }

    png_write_info(png_context, png_context_info);
    png_write_rows(png_context, gen_png_image_rows, gen_png_height);
    png_write_end(png_context, NULL);

    /* chardef */
    {
        const unsigned int unitsz = (2/*id*/+1/*x*/+1/*y*/+1/*w*/+1/*h*/+1/*xoffset*/+1/*yoffset*/+1/*xadvance*/); /* == 9 */
        const unsigned int sz = unitsz * chardef_count;

        assert(sz <= sizeof(temp));

        for (i=0;i < chardef_count;i++) {
            const struct chardef *cdef = &chardefs[i];
            unsigned char *d = temp + (i * unitsz);

            *((uint16_t*)(d+0)) = htole16(cdef->id);
            *((uint8_t *)(d+2)) = cdef->x;
            *((uint8_t *)(d+3)) = cdef->y;
            *((uint8_t *)(d+4)) = cdef->w;
            *((uint8_t *)(d+5)) = cdef->h;
            *((int8_t  *)(d+6)) = cdef->xoffset;
            *((int8_t  *)(d+7)) = cdef->yoffset;
            *((int8_t  *)(d+8)) = cdef->xadvance;
        }

        if (sz >= 96) {
            z_stream z;

            /* add a 16-bit unsigned int at the start so the program reading this
             * knows how much memory to allocate instead of guessing and possibly
             * causing buffer overruns or problems reading. */
            *((uint16_t*)(temp2+0)) = htole16(chardef_count);

            memset(&z,0,sizeof(z));
            z.next_in = temp;
            z.avail_in = sz;
            z.next_out = temp2+2;
            z.avail_out = sizeof(temp2)-2;
            if (deflateInit(&z, 9) != Z_OK) {
                fprintf(stderr,"Deflate error\n");
                return 1;
            }

            if (deflate(&z,Z_FINISH) != Z_STREAM_END) {
                fprintf(stderr,"Deflate error\n");
                return 1;
            }
            if (z.avail_in != 0) {
                fprintf(stderr,"Deflate ran out of room\n");
                return 1;
            }

            deflateEnd(&z);

            png_write_chunk(png_context, (png_const_bytep)("cDEZ"), temp2, (int)((unsigned char*)z.next_out - (unsigned char*)temp2));
        }
        else {
            png_write_chunk(png_context, (png_const_bytep)("cDEF"), temp, sz);
        }
    }

    /* kerndef */
    {
        const unsigned int unitsz = (2/*first*/+2/*second*/+1/*amount*/); /* == 5 */
        const unsigned int sz = unitsz * kerndef_count;

        assert(sz <= sizeof(temp));

        for (i=0;i < kerndef_count;i++) {
            const struct kerndef *kdef = &kerndefs[i];
            unsigned char *d = temp + (i * unitsz);

            *((uint16_t*)(d+0)) = htole16(kdef->first);
            *((uint16_t*)(d+2)) = htole16(kdef->second);
            *((int8_t  *)(d+4)) = kdef->amount;
        }

        if (sz >= 96) {
            z_stream z;

            /* add a 16-bit unsigned int at the start so the program reading this
             * knows how much memory to allocate instead of guessing and possibly
             * causing buffer overruns or problems reading. */
            *((uint16_t*)(temp2+0)) = htole16(kerndef_count);

            memset(&z,0,sizeof(z));
            z.next_in = temp;
            z.avail_in = sz;
            z.next_out = temp2+2;
            z.avail_out = sizeof(temp2)-2;
            if (deflateInit(&z, 9) != Z_OK) {
                fprintf(stderr,"Deflate error\n");
                return 1;
            }

            if (deflate(&z,Z_FINISH) != Z_STREAM_END) {
                fprintf(stderr,"Deflate error\n");
                return 1;
            }
            if (z.avail_in != 0) {
                fprintf(stderr,"Deflate ran out of room\n");
                return 1;
            }

            deflateEnd(&z);

            png_write_chunk(png_context, (png_const_bytep)("kDEZ"), temp2, (int)((unsigned char*)z.next_out - (unsigned char*)temp2));
        }
        else {
            png_write_chunk(png_context, (png_const_bytep)("kDEF"), temp, sz);
        }
    }

    /* success */
    ret = 0;
fail:
    if (png_context != NULL)
        png_destroy_write_struct(&png_context,&png_context_info);

    if (ret)
        fprintf(stderr,"Failed to save output PNG\n");

    fclose(fp);
    return ret;
}

int main(int argc,char **argv) {
    if (parse_argv(argc,argv))
        return 1;

    if (load_in_png())
        return 1;
    if (read_fnt())
        return 1;

    /* crop the PNG to the part actually used by the font */
    if (img_used_w != 0 && gen_png_width > img_used_w)
        gen_png_width = img_used_w;
    if (img_used_h != 0 && gen_png_height > img_used_h)
        gen_png_height = img_used_h;

    if (save_out_png())
        return 1;

    free_gen_png();
    return 0;
}
