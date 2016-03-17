
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
#include <hw/dos/doswin.h>
#include <hw/8254/8254.h>		/* 8254 timer */

#include <hw/mpu401/mpu401.h>

int main(int argc,char **argv) {
	probe_dos();
	init_mpu401();

	if (mpu401_try_base(0x300))
		fprintf(stderr,"Found MPU-401 at 300h\n");
	if (mpu401_try_base(0x330))
		fprintf(stderr,"Found MPU-401 at 330h\n");

	return 0;
}

