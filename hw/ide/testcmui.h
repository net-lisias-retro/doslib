
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>
#include <hw/ide/idelib.h>

#include "testutil.h"
#include "test.h"

void common_ide_success_or_error_vga_msg_box(struct ide_controller *ide,struct vga_msg_box *vgabox);
void common_failed_to_read_taskfile_vga_msg_box(struct vga_msg_box *vgabox);
void do_warn_if_atapi_not_in_data_input_state(struct ide_controller *ide);
void do_warn_if_atapi_not_in_complete_state(struct ide_controller *ide);
void do_warn_if_atapi_not_in_command_state(struct ide_controller *ide);

