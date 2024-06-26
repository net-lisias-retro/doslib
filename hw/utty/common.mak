# this makefile is included from all the dos*.mak files, do not use directly
# NTS: HPS is either \ (DOS) or / (Linux)

CFLAGS_THIS = -fr=nul -fo=$(SUBDIR)$(HPS).obj -i.. -i"../.."
NOW_BUILDING = HW_UTTY_LIB

TEST_EXE =     $(SUBDIR)$(HPS)test.$(EXEEXT)

$(HW_UTTY_LIB): $(SUBDIR)$(HPS)utty.obj $(SUBDIR)$(HPS)uttystr.obj $(SUBDIR)$(HPS)uttytmp.obj $(SUBDIR)$(HPS)uttyprna.obj $(SUBDIR)$(HPS)drv_pc98.obj $(SUBDIR)$(HPS)drv_vga.obj $(SUBDIR)$(HPS)uttycon.obj
	wlib -q -b -c $(HW_UTTY_LIB) -+$(SUBDIR)$(HPS)utty.obj -+$(SUBDIR)$(HPS)uttystr.obj -+$(SUBDIR)$(HPS)uttytmp.obj -+$(SUBDIR)$(HPS)uttyprna.obj -+$(SUBDIR)$(HPS)drv_pc98.obj -+$(SUBDIR)$(HPS)drv_vga.obj -+$(SUBDIR)$(HPS)uttycon.obj

# NTS we have to construct the command line into tmp.cmd because for MS-DOS
# systems all arguments would exceed the pitiful 128 char command line limit
.c.obj:
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	$(CC) @tmp.cmd
!ifdef TINYMODE
	$(OMFSEGDG) -i $@ -o $@
!endif

all: $(OMFSEGDG) lib exe
       
lib: $(HW_UTTY_LIB) .symbolic
	
exe: $(TEST_EXE) .symbolic

!ifdef TEST_EXE
$(TEST_EXE): $(HW_UTTY_LIB) $(HW_UTTY_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)test.obj
	%write tmp.cmd option quiet option map=$(TEST_EXE).map system $(WLINK_CON_SYSTEM) $(HW_UTTY_LIB_WLINK_LIBRARIES) file $(SUBDIR)$(HPS)test.obj name $(TEST_EXE)
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

clean: .SYMBOLIC
          del $(SUBDIR)$(HPS)*.obj
          del $(HW_UTTY_LIB)
          del tmp.cmd
          @echo Cleaning done

