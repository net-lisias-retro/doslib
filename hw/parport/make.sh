#!/usr/bin/bash
rel=../..
if [ x"$TOP" == x ]; then TOP=`pwd`/$rel; fi
. $rel/linux-ow.sh

dos=1 # MS-DOS
doshuge=1 # MS-DOS huge
dospc98=1 # MS-DOS PC98

if [ "$1" == "clean" ]; then
	do_clean
	rm -fv test.dsk test2.dsk nul.err tmp.cmd tmp1.cmd tmp2.cmd
	exit 0
fi

if [ "$1" == "disk" ]; then
	make_msdos_data_disk test.dsk || exit 1
	mcopy -i test.dsk dos86s/test.exe ::test86.exe
	mcopy -i test.dsk dos386f/test.exe ::test386.exe
	mcopy -i test.dsk ../../dos4gw.dat ::dos4gw.exe
	mcopy -i test.dsk dos86s/testpnp.exe ::tstp86.exe
	mcopy -i test.dsk dos386f/testpnp.exe ::tstp386.exe
	mcopy -i test.dsk dos86s/prnt.exe ::prnt86.exe
	mcopy -i test.dsk dos386f/prnt.exe ::prnt386.exe
	mcopy -i test.dsk dos86s/prntpnp.exe ::prnp86.exe
	mcopy -i test.dsk dos386f/prntpnp.exe ::prnp386.exe
	mcopy -i test.dsk dos86s/samptest.exe ::sampt86.exe
	mcopy -i test.dsk dos386f/samptest.exe ::sampt386.exe
	cp test.dsk test2.dsk
fi

if [[ "$1" == "build" || "$1" == "" ]]; then
	make_buildlist
	begin_bat

	what=all
	if [ x"$2" != x ]; then what="$2"; fi

	for name in $build_list; do
		do_wmake $name "$what" || exit 1
		bat_wmake $name "$what" || exit 1
	done

	end_bat
fi

