
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/8237/8237.h>		/* 8237 DMA */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/ide/idelib.h>
#include <hw/pci/pci.h>
#include <hw/dos/dos.h>

struct ide_controller		ide_controller[MAX_IDE_CONTROLLER];
int8_t				idelib_init = -1;

const struct ide_controller ide_isa_standard[4] = {
	/*base alt fired irq flags*/
	{0x1F0,0x3F6,0,14,{0}},
	{0x170,0x376,0,15,{0}},
	{0x1E8,0x3EE,0,11,{0}},	/* <- fixme: is this right? */
	{0x168,0x36E,0,10,{0}}	/* <- fixme; is this right? */
};

const struct ide_controller *idelib_get_standard_isa_port(int i) {
	if (i < 0 || i >= 4) return NULL;
	return &ide_isa_standard[i];
}

int init_idelib() {
	if (idelib_init < 0) {
		memset(ide_controller,0,sizeof(ide_controller));
		idelib_init = 0;

		cpu_probe();
		probe_dos();
		detect_windows();

		/* do NOT under any circumstances talk directly to IDE from under Windows! */
		if (windows_mode != WINDOWS_NONE) return (idelib_init=0);

		/* init OK */
		idelib_init = 1;
	}

	return (int)idelib_init;
}

void free_idelib() {
}

void idelib_controller_update_status(struct ide_controller *ide) {
	if (ide == NULL) return;
	ide->last_status = inp(ide->alt_io != 0 ? /*0x3F6-ish status*/ide->alt_io : /*status register*/(ide->base_io+7));

	/* if the IDE controller is NOT busy, then also note the status according to the selected drive's taskfile */
	if (!(ide->last_status&0x80)) ide->taskfile[ide->selected_drive].status = ide->last_status;
}

int idelib_controller_is_busy(struct ide_controller *ide) {
	if (ide == NULL) return 0;
	return !!(ide->last_status&0x80);
}

int idelib_controller_is_error(struct ide_controller *ide) {
	if (ide == NULL) return 0;
	return !!(ide->last_status&0x01) && !(ide->last_status&0x80)/*and not busy*/;
}

int idelib_controller_is_drq_ready(struct ide_controller *ide) {
	if (ide == NULL) return 0;
	return !!(ide->last_status&0x08) && !(ide->last_status&0x80)/*and not busy*/;
}

int idelib_controller_is_drive_ready(struct ide_controller *ide) {
	if (ide == NULL) return 0;
	return !!(ide->last_status&0x40) && !(ide->last_status&0x80)/*and not busy*/;
}

int idelib_controller_allocated(struct ide_controller *ide) {
	if (ide == NULL) return 0;
	return (ide->base_io != 0);
}

struct ide_controller *idelib_get_controller(int i) {
	if (i < 0 || i >= MAX_IDE_CONTROLLER) return NULL;
	if (!idelib_controller_allocated(&ide_controller[i])) return NULL;
	return &ide_controller[i];
}

struct ide_controller *idelib_new_controller() {
	int i;

	for (i=0;i < MAX_IDE_CONTROLLER;i++) {
		if (!idelib_controller_allocated(&ide_controller[i]))
			return &ide_controller[i];
	}

	return NULL;
}

struct ide_controller *idelib_by_base_io(uint16_t io) {
	int i;

	if (io == 0)
		return NULL;

	for (i=0;i < MAX_IDE_CONTROLLER;i++) {
		if (ide_controller[i].base_io == io)
			return &ide_controller[i];
	}

	return NULL;
}

struct ide_controller *idelib_by_alt_io(uint16_t io) {
	int i;

	if (io == 0)
		return NULL;

	for (i=0;i < MAX_IDE_CONTROLLER;i++) {
		if (ide_controller[i].alt_io == io)
			return &ide_controller[i];
	}

	return NULL;
}

struct ide_controller *idelib_by_irq(int8_t irq) {
	int i;

	if (irq < 0)
		return NULL;

	for (i=0;i < MAX_IDE_CONTROLLER;i++) {
		if (ide_controller[i].irq == irq)
			return &ide_controller[i];
	}

	return NULL;
}

struct ide_controller *idelib_probe(struct ide_controller *ide) {
	struct ide_controller *newide = NULL;
	uint16_t alt_io = 0;
	int8_t irq = -1;

	/* allocate a new empty slot. bail if all are full */
	newide = idelib_new_controller();
	if (newide == NULL)
		return NULL;

	/* we can work with the controller if alt_io is zero or irq < 0, but we
	 * require the base_io to be valid */
	if (ide->base_io == 0)
		return NULL;

	/* IDE I/O is always 8-port aligned */
	if ((ide->base_io & 7) != 0)
		return NULL;

	/* alt I/O if present is always 8-port aligned and at the 6th port */
	if (ide->alt_io != 0 && (ide->alt_io & 1) != 0)
		return NULL;

	/* don't probe if base io already taken */
	if (idelib_by_base_io(ide->base_io) != NULL)
		return NULL;

	irq = ide->irq;

	/* if the alt io conflicts, then don't use it */
	alt_io = ide->alt_io;
	if (alt_io != 0 && idelib_by_alt_io(alt_io) != NULL)
		alt_io = 0;

	/* the alt I/O port is supposed to be readable, and it usually exists as a
	 * mirror of what you read from the status port (+7). and there's no
	 * conceivable reason I know of that the controller would happen to be busy
	 * at this point in execution.
	 *
	 * NTS: we could reset the hard drives here as part of the test, but that might
	 *      not be wise in case the BIOS is cheap and sets up the controller only
	 *      once the way it expects.u */
	if (alt_io != 0 && inp(alt_io) == 0xFF)
		alt_io = 0;

	/* TODO: come up with a more comprehensive IDE I/O port test routine.
	 *       but one that reliably detects without changing hard disk state. */
	if (inp(ide->base_io+7) == 0xFF)
		return NULL;

	newide->irq_fired = 0;
	newide->irq = irq;
	newide->flags.io_irq_enable = (newide->irq >= 0) ? 1 : 0;	/* unless otherwise known, use the IRQ */
	newide->base_io = ide->base_io;
	newide->alt_io = alt_io;

	idelib_controller_update_taskfile(newide,0xFF/*all registers*/,0);

	newide->device_control = 0x08+(newide->flags.io_irq_enable?0x00:0x02); /* can't read device control but we can guess */
	if (ide->alt_io != 0) outp(ide->alt_io,newide->device_control);

	/* construct IDE taskfile from register contents */
	memset(&newide->taskfile,0,sizeof(newide->taskfile));
	newide->taskfile[newide->selected_drive].head_select = newide->head_select;

	return newide;
}

void ide_vlb_sync32_pio(struct ide_controller *ide) {
	inp(ide->base_io+2);
	inp(ide->base_io+2);
	inp(ide->base_io+2);
}

void idelib_controller_drive_select(struct ide_controller *ide,unsigned char which/*1=slave 0=master*/,unsigned char head/*CHS mode head value*/,unsigned char mode/*upper 3 bits*/) {
	if (ide == NULL) return;
	ide->head_select = ((which&1)<<4) + (head&0xF) + ((mode&7)<<5);
	outp(ide->base_io+6/*0x1F6*/,ide->head_select);

	/* and let it apply to whatever drive it selects */
	ide->selected_drive = (ide->head_select >> 4) & 1;
	ide->taskfile[ide->selected_drive].head_select = ide->head_select;
}

void idelib_enable_interrupt(struct ide_controller *ide,unsigned char en) {
	if (en) {
		if (!(ide->device_control&0x02)) /* if nIEN=0 already do nothing */
			return;

		ide->flags.io_irq_enable = 1;
		ide->irq_fired = 0;

		/* force clear IRQ */
		inp(ide->base_io+7);

		/* enable at IDE controller */
		if (ide->alt_io != 0) idelib_write_device_control(ide,0x08); /* nIEN=0 (enable) and not reset */
		else ide->device_control = 0x00; /* fake it */
	}
	else {
		if (ide->device_control&0x02) /* if nIEN=1 already do nothing */
			return;

		/* disable at IDE controller */
		if (ide->alt_io != 0) idelib_write_device_control(ide,0x08+0x02); /* nIEN=1 (disable) and not reset */
		else ide->device_control = 0x02; /* fake it */

		ide->flags.io_irq_enable = 0;
		ide->irq_fired = 0;
	}
}

int idelib_controller_apply_taskfile(struct ide_controller *ide,unsigned char portmask,unsigned char flags) {
	if (portmask & 0x80/*base+7*/) {
		/* if the IDE controller is busy we cannot apply the taskfile */
		ide->last_status = inp(ide->alt_io != 0 ? /*0x3F6-ish status*/ide->alt_io : /*status register*/(ide->base_io+7));
		if (ide->last_status&0x80) return -1; /* if the controller is busy, then the other registers have no meaning */
	}
	if (portmask & 0x40/*base+6*/) {
		/* we do not write the head/drive select/mode here but we do note what was written */
		/* if writing the taskfile would select a different drive or head, then error out */
		ide->head_select = inp(ide->base_io+6);
		if (ide->selected_drive != ((ide->head_select >> 4) & 1)) return -1;
		if ((ide->head_select&0x1F) != (ide->taskfile[ide->selected_drive].head_select&0x1F)) return -1;
		outp(ide->base_io+6,ide->taskfile[ide->selected_drive].head_select);
	}

	if (flags&IDELIB_TASKFILE_LBA48_UPDATE)
		ide->taskfile[ide->selected_drive].assume_lba48 = (flags&IDELIB_TASKFILE_LBA48)?1:0;

	/* and read back the upper 3 bits for the current mode, to detect LBA48 commands.
	 * NTS: Unfortunately some implementations, even though you wrote 0x4x, will return 0xEx regardless,
	 *      so it's not really that easy. In that case, the only way to know is if the flags are set
	 *      in the taskfile indicating that an LBA48 command was issued. */
	if (ide->taskfile[ide->selected_drive].assume_lba48/*we KNOW we issued an LBA48 command*/ ||
		(ide->head_select&0xE0) == 0x40/*LBA48 command was issued (NTS: but most IDE controllers replace with 0xE0, so...)*/) {
		/* write 16 bits to 0x1F2-0x1F5 */
		if (portmask & 0x04) {
			outp(ide->base_io+2,ide->taskfile[ide->selected_drive].sector_count>>8);
			outp(ide->base_io+2,ide->taskfile[ide->selected_drive].sector_count);
		}
		if (portmask & 0x08) {
			outp(ide->base_io+3,ide->taskfile[ide->selected_drive].lba0_3>>8);
			outp(ide->base_io+3,ide->taskfile[ide->selected_drive].lba0_3);
		}
		if (portmask & 0x10) {
			outp(ide->base_io+4,ide->taskfile[ide->selected_drive].lba1_4>>8);
			outp(ide->base_io+4,ide->taskfile[ide->selected_drive].lba1_4);
		}
		if (portmask & 0x20) {
			outp(ide->base_io+5,ide->taskfile[ide->selected_drive].lba2_5>>8);
			outp(ide->base_io+5,ide->taskfile[ide->selected_drive].lba2_5);
		}
	}
	else {
		if (portmask & 0x04)
			outp(ide->base_io+2,ide->taskfile[ide->selected_drive].sector_count);
		if (portmask & 0x08)
			outp(ide->base_io+3,ide->taskfile[ide->selected_drive].lba0_3);
		if (portmask & 0x10)
			outp(ide->base_io+4,ide->taskfile[ide->selected_drive].lba1_4);
		if (portmask & 0x20)	
			outp(ide->base_io+5,ide->taskfile[ide->selected_drive].lba2_5);
	}

	if (portmask & 0x02)
		outp(ide->base_io+1,ide->taskfile[ide->selected_drive].features);

	/* and finally, the command */
	if (portmask & 0x80/*base+7*/) {
		outp(ide->base_io+7,ide->taskfile[ide->selected_drive].command);
		ide->last_status = inp(ide->alt_io != 0 ? /*0x3F6-ish status*/ide->alt_io : /*status register*/(ide->base_io+7));
	}

	return 0;
}

int idelib_controller_update_taskfile(struct ide_controller *ide,unsigned char portmask,unsigned char flags) {
	if (portmask & 0x80/*base+7*/) {
		ide->last_status = inp(ide->alt_io != 0 ? /*0x3F6-ish status*/ide->alt_io : /*status register*/(ide->base_io+7));
		if (ide->last_status&0x80) return -1; /* if the controller is busy, then the other registers have no meaning */
	}

	if (portmask & 0x40/*base+6*/) {
		/* read drive select, use it to determine who is selected */
		ide->head_select = inp(ide->base_io+6);
		ide->selected_drive = (ide->head_select >> 4) & 1;
		ide->taskfile[ide->selected_drive].head_select = ide->head_select;
	}

	if (flags&IDELIB_TASKFILE_LBA48_UPDATE)
		ide->taskfile[ide->selected_drive].assume_lba48 = (flags&IDELIB_TASKFILE_LBA48)?1:0;

	/* and read back the upper 3 bits for the current mode, to detect LBA48 commands.
	 * NTS: Unfortunately some implementations, even though you wrote 0x4x, will return 0xEx regardless,
	 *      so it's not really that easy. In that case, the only way to know is if the flags are set
	 *      in the taskfile indicating that an LBA48 command was issued. the program using this function
	 *      will presumably let us know if it is by sending:
	 *      flags = IDELIB_TASKFILE_LBA48 | IDELIB_TASKFILE_LBA48_UPDATE */
	if (ide->taskfile[ide->selected_drive].assume_lba48/*we KNOW we issued an LBA48 command*/ ||
		(ide->head_select&0xE0) == 0x40/*LBA48 command was issued*/) {
		/* read back 16 bits from 0x1F2-0x1F5 */
		if (portmask & 0x04) {
			ide->taskfile[ide->selected_drive].sector_count  = (uint16_t)inp(ide->base_io+2) << 8;
			ide->taskfile[ide->selected_drive].sector_count |= (uint16_t)inp(ide->base_io+2);
		}
		if (portmask & 0x08) {
			ide->taskfile[ide->selected_drive].lba0_3  = (uint16_t)inp(ide->base_io+3) << 8;
			ide->taskfile[ide->selected_drive].lba0_3 |= (uint16_t)inp(ide->base_io+3);
		}
		if (portmask & 0x10) {
			ide->taskfile[ide->selected_drive].lba1_4  = (uint16_t)inp(ide->base_io+4) << 8;
			ide->taskfile[ide->selected_drive].lba1_4 |= (uint16_t)inp(ide->base_io+4);
		}
		if (portmask & 0x20) {
			ide->taskfile[ide->selected_drive].lba2_5  = (uint16_t)inp(ide->base_io+5) << 8;
			ide->taskfile[ide->selected_drive].lba2_5 |= (uint16_t)inp(ide->base_io+5);
		}
	}
	else {
		if (portmask & 0x04)
			ide->taskfile[ide->selected_drive].sector_count = (uint16_t)inp(ide->base_io+2);
		if (portmask & 0x08)
			ide->taskfile[ide->selected_drive].lba0_3 = (uint16_t)inp(ide->base_io+3);
		if (portmask & 0x10)
			ide->taskfile[ide->selected_drive].lba1_4 = (uint16_t)inp(ide->base_io+4);
		if (portmask & 0x20)
			ide->taskfile[ide->selected_drive].lba2_5 = (uint16_t)inp(ide->base_io+5);
	}

	if (portmask & 0x02)
		ide->taskfile[ide->selected_drive].error = (uint16_t)inp(ide->base_io+1);

	return 0;
}

struct ide_taskfile *idelib_controller_get_taskfile(struct ide_controller *ide,int which) {
	if (which < 0) which = ide->selected_drive;
	else if (which >= 2) return NULL;
	return &ide->taskfile[which];
}

