
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
#include <hw/8237/8237.h>
#include <hw/8254/8254.h>
#include <hw/8259/8259.h>
#include <hw/sndsb/sndsb.h>
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

struct sndsb_ctx*       sound_blaster_ctx = NULL;

unsigned char           sound_blaster_irq_hook = 0;
unsigned char           sound_blaster_old_irq_masked = 0; /* very important! If the IRQ was masked prior to running this program there's probably a good reason */
void                    (interrupt *sound_blaster_old_irq)() = NULL;

void interrupt sound_blaster_irq() {
	unsigned char c;

	sound_blaster_ctx->irq_counter++;

	/* ack soundblaster DSP if DSP was the cause of the interrupt */
	/* NTS: Experience says if you ack the wrong event on DSP 4.xx it
	   will just re-fire the IRQ until you ack it correctly...
	   or until your program crashes from stack overflow, whichever
	   comes first */
	c = sndsb_interrupt_reason(sound_blaster_ctx);
	sndsb_interrupt_ack(sound_blaster_ctx,c);

	/* FIXME: The sndsb library should NOT do anything in
	   send_buffer_again() if it knows playback has not started! */
	/* for non-auto-init modes, start another buffer */
	sndsb_irq_continue(sound_blaster_ctx,c);

	/* NTS: we assume that if the IRQ was masked when we took it, that we must not
	 *      chain to the previous IRQ handler. This is very important considering
	 *      that on most DOS systems an IRQ is masked for a very good reason---the
	 *      interrupt handler doesn't exist! In fact, the IRQ vector could easily
	 *      be unitialized or 0000:0000 for it! CALLing to that address is obviously
	 *      not advised! */
	if (sound_blaster_old_irq_masked || sound_blaster_old_irq == NULL) {
		/* ack the interrupt ourself, do not chain */
		if (sound_blaster_ctx->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
	}
	else {
		/* chain to the previous IRQ, who will acknowledge the interrupt */
		sound_blaster_old_irq();
	}
}

void sound_blaster_unhook_irq(void) {
    if (sound_blaster_irq_hook && sound_blaster_ctx != NULL) {
        if (sound_blaster_ctx->irq >= 0) {
            /* If the IRQ was masked on hooking, then mask the IRQ again */
            if (sound_blaster_old_irq_masked)
                p8259_mask(sound_blaster_ctx->irq);

            /* Restore the old IRQ handler */
            _dos_setvect(irq2int(sound_blaster_ctx->irq),sound_blaster_old_irq);
        }

        sound_blaster_old_irq = NULL;
        sound_blaster_irq_hook = 0;
    }
}

void sound_blaster_hook_irq(void) {
    if (!sound_blaster_irq_hook && sound_blaster_ctx != NULL) {
        if (sound_blaster_ctx->irq >= 0) {
            /* If the IRQ was masked on entry, there's probably a good reason for it, such as
             * a NULL vector, a BIOS (or DOSBox) with just an IRET instruction that doesn't
             * acknowledge the interrupt, or perhaps some junk. Whatever the reason, take it
             * as a sign not to chain to the previous interrupt handler. */
            sound_blaster_old_irq_masked = p8259_is_masked(sound_blaster_ctx->irq);
            if (vector_is_iret(irq2int(sound_blaster_ctx->irq)))
                sound_blaster_old_irq_masked = 1;

            /* hook the IRQ, install our own, then unmask the IRQ */
            sound_blaster_irq_hook = 1;
            sound_blaster_old_irq = _dos_getvect(irq2int(sound_blaster_ctx->irq));
            _dos_setvect(irq2int(sound_blaster_ctx->irq),sound_blaster_irq);
            p8259_unmask(sound_blaster_ctx->irq);
        }
    }
}

void my_unhook_irq(void) {
    sound_blaster_unhook_irq();
}

void gen_res_free(void) {
//    seq_com_cleanup();
//    sin2048fps16_free();
//    font_bmp_free(&arial_small);
//    font_bmp_free(&arial_medium);
//    font_bmp_free(&arial_large);
//    dumbpack_close(&sorc_pack);

    if (sound_blaster_ctx != NULL) {
        sndsb_stop_dsp_playback(sound_blaster_ctx);
        sound_blaster_unhook_irq();

        /* NTS: This erases the sound card from the list, zeros the struct! */
        sndsb_free_card(sound_blaster_ctx);
        sound_blaster_ctx = NULL;
    }
}

static struct minipng_reader *woo_title_load_png(unsigned char *buf,unsigned int w,unsigned int h,const char *path) {
    struct minipng_reader *rdr;
    unsigned int y;

    if ((rdr=minipng_reader_open(path)) == NULL)
        return NULL;

    if (minipng_reader_parse_head(rdr) || rdr->plte == NULL || rdr->plte_count == 0 || rdr->ihdr.width != w || rdr->ihdr.height != h) {
        minipng_reader_close(&rdr);
        return NULL;
    }

    for (y=0;y < h;y++) {
        unsigned char *imgptr = buf + (y * w);

        if (minipng_reader_read_idat(rdr,imgptr,1) != 1) { /* pad byte */
            minipng_reader_close(&rdr);
            return NULL;
        }

        if (rdr->ihdr.bit_depth == 8) {
            if (minipng_reader_read_idat(rdr,imgptr,w) != w) { /* row */
                minipng_reader_close(&rdr);
                return NULL;
            }
        }
        else if (rdr->ihdr.bit_depth == 4) {
            if (minipng_reader_read_idat(rdr,imgptr,(w+1u)>>1u) != ((w+1u)>>1u)) { /* row */
                minipng_reader_close(&rdr);
                return NULL;
            }
            minipng_expand4to8(imgptr,w);
        }
    }

    return rdr;
}

static void woo_title_display(unsigned char *imgbuf,unsigned int w,unsigned int h,const char *path) {
    struct minipng_reader *rdr;
    unsigned char *dp,*sp;
    unsigned int i,j,c;

    rdr = woo_title_load_png(imgbuf,w,h,path);
    if (rdr == NULL) fatal("woo_title title %s",path);

    /* set the VGA palette first. this will deliberately cause a palette flash and corruption
     * of the old image's palette. furthermore we purposely draw it on screen slowly line by
     * line. Because Mr. Wooo. Sorcerer programming mistakes and slow inefficient code. */
    if (rdr->plte != NULL) {
        vga_palette_lseek(0);
        for (i=0;i < rdr->plte_count;i++)
            vga_palette_write(rdr->plte[i].red>>2,rdr->plte[i].green>>2,rdr->plte[i].blue>>2);
    }

    /* 4-pixel rendering. This code is still using VGA unchained 256-color mode. */
    for (i=0;i < h;i++) {
        sp = imgbuf + (i * w);

        for (c=0;c < 4;c++) {
            dp = vga_state.vga_graphics_ram + (i * 80u);
            vga_write_sequencer(0x02/*map mask*/,1u << c);
            for (j=c;j < w;j += 4) *dp++ = sp[j];
        }

        if ((i&7) == 6) {
            vga_wait_for_vsync();
            vga_wait_for_vsync_end();
        }
    }

    minipng_reader_close(&rdr);
}

void woo_title(void) {
    unsigned char *imgbuf;
    uint32_t now,next;
    int c;

    imgbuf = malloc(320u*200u); // 64000 bytes
    if (imgbuf == NULL) fatal("woo_title imgbuf NULL");

    /* as part of the gag, set the VGA mode X rendering to draw on active display.
     * furthermore the code is written to set palette, then draw the code with
     * deliberately poor performance. Going from one title image to another this
     * causes a palette flash inbetween. The visual gag here is that Mr. Wooo. Sorcerer
     * isn't the expert he thinks he is and makes some n00b programming mistakes like
     * that. */
    vga_cur_page = vga_next_page = VGA_PAGE_FIRST;
    vga_state.vga_graphics_ram = orig_vga_graphics_ram + vga_next_page;
    vga_set_start_location(vga_cur_page);

    woo_title_display(imgbuf,320,200,"cr52ti1.png");
    now = read_timer_counter();
    next = now + (120u * 5u);
    do {
        now = read_timer_counter();
        if (kbhit()) {
            c = getch();
            if (c == 27) {
                goto finishnow;
            }
            else if (c == ' ') {
                next = now;
            }
        }
    } while (now < next);

    woo_title_display(imgbuf,320,200,"cr52ti2.png");
    now = read_timer_counter();
    next = now + (120u * 6u);
    do {
        now = read_timer_counter();
        if (kbhit()) {
            c = getch();
            if (c == 27) {
                goto finishnow;
            }
            else if (c == ' ') {
                next = now;
            }
        }
    } while (now < next);

finishnow:
    free(imgbuf);
}

/* Sound Blaster detection using hw/sndsb */
void detect_sound_blaster(void) {
    struct sndsb_ctx* ctx;

    /* First allow the user to control our detection with SET BLASTER=... in the environment block.
     * Since DOSBox/DOSBox-X usually sets BLASTER this means we'll use whatever I/O port, IRQ, and DMA
     * they assigned in dosbox.conf as well. */
    if (sndsb_index_to_ctx(0)->baseio == 0/*NTS: currently does not return NULL*/ && sndsb_try_blaster_var() != NULL) {
        if (!sndsb_init_card(sndsb_card_blaster))
            sndsb_free_card(sndsb_card_blaster);
    }

    /* Otherwise, try the usual port 220h and port 240h most Sound Blaster cards are configured on,
     * but only if we didn't get anything with SET BLASTER=...  Port 220h is VERY COMMON, Port 240h
     * is much less common. */
    if (sndsb_index_to_ctx(0)->baseio == 0/*NTS: currently does not return NULL*/)
        sndsb_try_base(0x220);
    if (sndsb_index_to_ctx(0)->baseio == 0/*NTS: currently does not return NULL*/)
        sndsb_try_base(0x240);

    /* Stop here if none detected */
    if ((ctx=sndsb_index_to_ctx(0))->baseio == 0/*NTS: currently does not return NULL*/)
        return;

    printf("Possible Sound Blaster detected at I/O port %xh\n",ctx->baseio);

    /* Autodetect the IRQ and DMA if not already obtained from SET BLASTER... */
    if (ctx->irq < 0)
        sndsb_probe_irq_F2(ctx);
    if (ctx->irq < 0)
        sndsb_probe_irq_80(ctx);
    if (ctx->dma8 < 0)
        sndsb_probe_dma8_E2(ctx);
    if (ctx->dma8 < 0)
        sndsb_probe_dma8_14(ctx);

    /* No IRQ/DMA, no sound. Not doing Goldplay or Direct DAC in *this* game, sorry */
    if (ctx->irq < 0 || ctx->dma8 < 0)
        return;

    /* Check card capabilities */
    sndsb_update_capabilities(ctx);
    sndsb_determine_ideal_dsp_play_method(ctx);

    /* Ok, accept */
    sound_blaster_ctx = ctx;

    printf("Found Sound Blaster at %xh IRQ %d DMA %d\n",sound_blaster_ctx->baseio,sound_blaster_ctx->irq,sound_blaster_ctx->dma8);

    sound_blaster_hook_irq();

    t8254_wait(t8254_us2ticks(1000000)); /* 1 second */
}

int main(int argc,char **argv) {
    (void)argc;
    (void)argv;

    probe_dos();
	cpu_probe();
    if (cpu_basic_level < 3) {
        printf("This game requires a 386 or higher\n");
        return 1;
    }

	if (!probe_8237()) {
		printf("Chip not present. Your computer might be 2010-era hardware that dropped support for it.\n");
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
    if (!init_sndsb()) {
        printf("Sound Blaster lib init fail.\n");
        return 1;
    }

    other_unhook_irq = my_unhook_irq;

    write_8254_system_timer(0);

    detect_keyboard();
    detect_sound_blaster();

#if TARGET_MSDOS == 16
# if 0 // not using it yet
    probe_emm();            // expanded memory support
    probe_himem_sys();      // extended memory support
# endif
#endif

    init_timer_irq();
    init_vga256unchained();

    woo_title();

    gen_res_free();
    check_heap();
    unhook_irqs();
    restore_text_mode();

    //debug
    dbg_heap_list();

    return 0;
}

