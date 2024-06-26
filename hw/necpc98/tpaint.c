 
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/dos/dos.h>
#include <hw/8254/8254.h>
#include <hw/necpc98/necpc98.h>

#if TARGET_MSDOS == 32
unsigned short *TRAM_C = (unsigned short*)0xA0000;
unsigned short *TRAM_A = (unsigned short*)0xA2000;
#else
unsigned short far *TRAM_C = (unsigned short far*)0xA0000000;
unsigned short far *TRAM_A = (unsigned short far*)0xA2000000;
#endif

unsigned short gdc_base = 0x60;

static char tmp[256];

static inline void gdc_write_command(const unsigned char cmd) {
    while (inp(gdc_base) & 0x02); /* while FIFO full */
    outp(gdc_base+2,cmd); // write command
}

static inline void gdc_write_data(const unsigned char d) {
    while (inp(gdc_base) & 0x02); /* while FIFO full */
    outp(gdc_base,d); // write data
}

int main(int argc,char **argv) {
    uint16_t charcode = 0x2106; /* double-wide A */
    uint8_t attrcode = 0xE1; /* white */
    unsigned char rowheight = 16;
    unsigned char rows = 25;
    unsigned char simplegraphics = 0;
    unsigned char cursx = 0,cursy = 0;
    unsigned int cur_x = 0,cur_y = 0;
    unsigned char running = 1;
    unsigned char redraw = 1;
    unsigned char sgdraw = 0;
    int c;

    /* NTS: Generally, if you write a doublewide character, the code is taken
     *      from that cell and continued over the next cell. The next cell
     *      contents are ignored. Except... that doesn't seem to be strictly
     *      true for ALL codes. Some codes are fullwidth but will show only
     *      the first half unless you write the same code twice. Some codes
     *      you think would make a doublewide code but don't. I'm not sure why.
     *
     *      If you see half of a character, try hitting 'f' to toggle filling
     *      both cells with the same code. */

	printf("NEC PC-98 doslib test program\n");
	if (!probe_nec_pc98()) {
		printf("Sorry, your system is not PC-98\n");
		return 1;
	}

    {
        unsigned char c = 16;

        __asm {
            mov     ah,0x0B
            int     18h
            mov     c,al
        }

        if (c & 1) {
            rowheight = 20;
            rows = 20;
        }
    }

    printf("\x1B[m"); /* normal attributes */
    printf("\x1B[J"); /* clear screen */
    printf("\x1B[1>h"); /* hide function row */
    printf("\x1B[5>h"); /* hide cursor (so we can directly control it) */
    fflush(stdout);

    while (running) {
        if (redraw) {
            sprintf(tmp,"X=%02u Y=%02u char=0x%04x attr=0x%02x sg=%u sgd=%u",cur_x,cur_y,charcode,attrcode,simplegraphics,sgdraw);
            {
                const unsigned int ofs = 80*(rows-1);
                unsigned int i = 0;
                char *s = tmp;

                for (;i < 78;i++) {
                    char c = *s;

                    TRAM_C[i+ofs] = (uint16_t)c;
                    TRAM_A[i+ofs] = 0xE5; /* white, invert */

                    if (c != 0) s++;
                }

                TRAM_C[78+ofs] = charcode; TRAM_A[78+ofs] = 0xE5;
                TRAM_C[79+ofs] = charcode; TRAM_A[79+ofs] = 0xE5;
            }

            {
                unsigned char visible = sgdraw ? 0 : 1;
                unsigned int addr = (cur_y * 80) + cur_x;

                gdc_write_command(0x49); /* cursor position */
                gdc_write_data(addr & 0xFF);
                gdc_write_data((addr >> 8) & 0xFF);
                gdc_write_data(0);

                gdc_write_command(0x4B); /* cursor setup */
                gdc_write_data((visible ? 0x80 : 0x00) + (rowheight - 1)); /* visible, 16 lines */
                gdc_write_data(0x00);                   /* BR(L 2-bit) = 3 SC = 0 (blink) CTOP = 0 */
                gdc_write_data(((rowheight - 1) << 3) + 0x04); /* CBOT, BR(U) = 4 */
            }

            redraw = 0;
        }

        c = getch();
        if (c == 27) break;

        if (c == 0x08/*left arrow*/) {
            if (sgdraw && (cursx & 1) != 0) {
                cursx--;
            }
            else if (cur_x > 0) {
                cursx=1;
                cur_x--;
                redraw = 1;
            }
        }
        else if (c == 0x0C/*right arrow*/) {
            if (sgdraw && (cursx & 1) != 1) {
                cursx++;
            }
            else if (cur_x < 79) {
                cursx=0;
                cur_x++;
                redraw = 1;
            }
        }
        else if (c == 0x0B/*up arrow*/) {
            if (sgdraw && (cursy & 3) != 0) {
                cursy--;
            }
            else if (cur_y > 0) {
                cursy=3;
                cur_y--;
                redraw = 1;
            }
        }
        else if (c == 0x0A/*down arrow*/) {
            if (sgdraw && (cursy & 3) != 3) {
                cursy++;
            }
            else if (cur_y < (rows-1-1)) {
                cursy=0;
                cur_y++;
                redraw = 1;
            }
        }
        else if (c == 'g') {
            simplegraphics ^= 1;
            redraw = 1;

            outp(0x68,simplegraphics ? 0x01 : 0x00);
        }
        else if (c == 'C' || c == 'c') {
            charcode += (c == 'C' ? 0x0100 : 0xFF00);
            redraw = 1;
        }
        else if (c == 'V' || c == 'v') {
            charcode += (c == 'V' ? 0x0001 : 0xFFFF);
            redraw = 1;
        }
        else if (c >= '1' && c <= '8') {
            attrcode ^= 1U << ((unsigned char)(c - '1'));
            redraw = 1;
        }
        else if (c == ' ') {
            unsigned int addr = (cur_y * 80) + cur_x;

            if (sgdraw) {
                /* NTS: "Simple graphics" means the lower 8 bits are arranged on the screen
                 *      in this bit order like a low res bitmap.
                 *
                 *      0 4
                 *      1 5
                 *      2 6
                 *      3 7
                 */
                uint16_t flipbit = 1u << (cursy + (cursx<<2u));

                if (!(TRAM_A[addr] & 0x10u)) {
                    TRAM_C[addr] = 0x00;
                    TRAM_A[addr] = attrcode | 0x10u;
                }

                TRAM_C[addr] ^= flipbit;
                TRAM_A[addr] |= 0x10u;
            }
            else {
                TRAM_C[addr] = charcode;
                TRAM_A[addr] = attrcode;
            }
        }
        else if (c == 9) {
            unsigned int addr = (cur_y * 80) + cur_x;

            TRAM_C[addr] = 0;
        }
        else if (c == '!') { /* 20/25-line switch */
            unsigned char c = 0;

            __asm {
                mov     ah,0x0B         ; get CRT mode
                int     18h
                xor     al,0x01         ; toggle 20/25-line mode
                mov     c,al
                mov     ah,0x0A
                int     18h
            }

            rowheight = (c & 1) ? 20 : 16;
            rows = (c & 1) ? 20 : 25;
            redraw = 1;

            if (rows == 25) {
                /* need to clear rows 19-24 */
                unsigned int i = 80*19;
                unsigned int j = 80*25;

                while (i < j) {
                    TRAM_C[i] = 0;
                    TRAM_A[i] = 0xE1;
                    i++;
                }
            }
        }
        else if (c == '(') {
            if (rowheight > 1) {
                rowheight--;
                redraw = 1;

                if (rowheight > 0x10) {
                    outp(0x74,0x10);
                    outp(0x72,0x7u + ((rowheight + 1u) >> 1u));
                    outp(0x70,0x8u - (rowheight >> 1u)); /* 2's complement */
                }
                else {
                    outp(0x74,rowheight);
                    outp(0x72,rowheight - 1);
                    outp(0x70,0);
                }

                rows = 400 / rowheight;
                if (rows > (0xF80 / 80))
                    rows = (0xF80 / 80);
            }
        }
        else if (c == ')') {
            if (rowheight < 0x20) {
                rowheight++;
                redraw = 1;

                if (rowheight > 0x10) {
                    outp(0x74,0x10);
                    outp(0x72,0x7u + ((rowheight + 1u) >> 1u));
                    outp(0x70,0x8u - (rowheight >> 1u)); /* 2's complement */
                }
                else {
                    outp(0x74,rowheight);
                    outp(0x72,rowheight - 1);
                    outp(0x70,0);
                }

                rows = 400 / rowheight;
                if (rows > (0xF80 / 80))
                    rows = (0xF80 / 80);
            }
        }
        else if (c == '%') {
            unsigned char recomp_row = 0;
            unsigned char e;

            c = getch();
            if (c == 'a' || c == 'A') { /* % a play with port 70h */
                e = inp(0x70);
                e += (c == 'A' ? 0x01 : 0xFF);
                outp(0x70,e);
                recomp_row = 1;
            }
            else if (c == 's' || c == 'S') { /* % s play with port 72h */
                e = inp(0x72);
                e += (c == 'S' ? 0x01 : 0xFF);
                outp(0x72,e);
                recomp_row = 1;
            }
            else if (c == 'd' || c == 'D') { /* % s play with port 74h */
                e = inp(0x74);
                e += (c == 'D' ? 0x01 : 0xFF);
                outp(0x74,e);
            }
            else if (c == 'g' || c == 'G') {
                sgdraw = !sgdraw;
                redraw = 1;

                if (sgdraw) {
                    cursx = 0;
                    cursy = 0;
                }

                if (!simplegraphics) {
                    simplegraphics ^= 1;
                    outp(0x68,simplegraphics ? 0x01 : 0x00);
                }
            }

            if (recomp_row) {
                e = (inp(0x72) & 0x1F);
                e -= (inp(0x70) & 0x1F);
                if (e & 0x80) e += 0x20;
                e++;
                assert(e <= 0x20);

                rowheight = e;
                rows = 400 / rowheight;
                if (rows > (0xF80 / 80))
                    rows = (0xF80 / 80);

                redraw = 1;
            }
        }
    }

    __asm {
        mov     ah,0x0B         ; get CRT mode
        int     18h
        and     al,0xFE         ; 25-line mode
        mov     ah,0x0A
        int     18h
    }

    printf("\n");
    printf("\x1B[1>l"); /* show function row */
    printf("\x1B[5>l"); /* show cursor */
    fflush(stdout);

	return 0;
}

