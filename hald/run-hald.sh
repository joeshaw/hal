#!/bin/sh

information_fdidir="../../hal-info/fdi"

case `uname -s` in
    FreeBSD)	backend=freebsd ;;
    SunOS)	backend=solaris ;;
    *)		backend=linux ;;
esac
export HALD_RUNNER_PATH=`pwd`/$backend:`pwd`/$backend/probing:`pwd`/$backend/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/$backend
export PATH=`pwd`/../hald-runner:$PATH

if [ "$1" = "--skip-fdi-install" ] ; then
    shift
else
    rm -rf .local-fdi
    make -C ../privileges install DESTDIR=`pwd`/.local-fdi prefix=/

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
export HAL_FDI_CACHE_NAME=.local-fdi/hald-local-fdi-cache
export POLKIT_PRIVILEGE_DIR=`pwd`/.local-fdi/etc/PolicyKit/privileges

./hald --daemon=no --verbose=yes $@
#./hald --daemon=no


