#!/bin/bash
rel=../..
if [ x"$TOP" == x ]; then TOP=`pwd`/$rel; fi
. $rel/linux-ow.sh

win31=1 # Windows 3.1

if [ "$1" == "clean" ]; then
    do_clean
    rm -fv test.dsk
    exit 0
fi

if [ "$1" == "disk" ]; then
    make_msdos_data_disk test.dsk || exit 1
    mcopy -i test.dsk win313l/hello.exe ::hello.exe
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

