#!/bin/bash
rel=../..
if [ x"$TOP" == x ]; then TOP=`pwd`/$rel; fi
. $rel/linux-ow.sh

dos=1 # MS-DOS
dostiny=1 # MS-DOS tiny model
doshuge=1 # MS-DOS huge
dospc98=1 # MS-DOS PC98

if [ "$1" == "clean" ]; then
    do_clean
    rm -fv test.dsk test2.dsk test98.dsk nul.err tmp.cmd tmp1.cmd tmp2.cmd
    exit 0
fi

if [ "$1" == "disk" ]; then
    make_msdos_data_disk test.dsk || exit 1
    mcopy -i test.dsk dos86c/test.exe ::test86.exe
    mcopy -i test.dsk dos386f/test.exe ::test386.exe
    gunzip -c -d ../necpc98/necpc98.fd0.gz >test98.dsk
    mcopy -i test98.dsk d9886s/test.exe ::test98s.exe
    mcopy -i test98.dsk d9886l/test.exe ::test98l.exe
fi

if [[ "$1" == "build" || "$1" == "" ]]; then
    make_buildlist
    begin_bat

    what=all
    if [ x"$2" != x ]; then what="$2"; fi
    if [ x"$3" != x ]; then build_list="$3"; fi

    for name in $build_list; do
        do_wmake $name "$what" || exit 1
        bat_wmake $name "$what" || exit 1
    done

    end_bat
fi

