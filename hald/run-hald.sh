#!/bin/sh

information_fdidir="../../hal-info/fdi"

export HALD_RUNNER_PATH=`pwd`/linux:`pwd`/linux/probing:`pwd`/linux/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/linux
export PATH=`pwd`/../hald-runner:$PATH

if [ "$1" = "--skip-fdi-install" ] ; then
    shift
else
    rm -rf .local-fdi
    make -C ../fdi install DESTDIR=`pwd`/.local-fdi prefix=/ && \
    if [ ! -d $information_fdidir ] ; then
    	echo "ERROR: You need to checkout hal-info in the same level"
    	echo "directory as hal to get the information fdi files."
    	exit
    fi
    make -C $information_fdidir install DESTDIR=`pwd`/.local-fdi prefix=/
fi
export HAL_FDI_SOURCE_PREPROBE=.local-fdi/share/hal/fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=.local-fdi/share/hal/fdi/information
export HAL_FDI_SOURCE_POLICY=.local-fdi/share/hal/fdi/policy

./hald --daemon=no --verbose=yes $@
#./hald --daemon=no


