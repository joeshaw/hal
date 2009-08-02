#!/bin/sh

information_fdidir="../../hal-info/fdi"

case `uname -s` in
    FreeBSD)	backend=freebsd ;;
    SunOS)	backend=solaris ;;
    *)		backend=linux ;;
esac
export HALD_RUNNER_PATH=`pwd`/$backend:`pwd`/$backend/probing:`pwd`/$backend/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/$backend
export PATH=`pwd`/../hald-runner:$PATH

HALD_TMPDIR=/tmp/run-hald-$USER

if [ "$1" = "--skip-fdi-install" ] ; then
    shift
else
    rm -rf $HALD_TMPDIR
    mkdir -p $HALD_TMPDIR
    make -C ../fdi install DESTDIR=$HALD_TMPDIR prefix=/ && \
    make -C ../policy install DESTDIR=$HALD_TMPDIR prefix=/
    if [ ! -d $information_fdidir ] ; then
    	echo "ERROR: You need to checkout hal-info in the same level"
    	echo "directory as hal to get the information fdi files."
    	exit
    fi
    make -C $information_fdidir install DESTDIR=$HALD_TMPDIR prefix=/
fi
export HAL_FDI_SOURCE_PREPROBE=$HALD_TMPDIR/share/hal/fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=$HALD_TMPDIR/share/hal/fdi/information
export HAL_FDI_SOURCE_POLICY=$HALD_TMPDIR/share/hal/fdi/policy
export HAL_FDI_CACHE_NAME=$HALD_TMPDIR/hald-local-fdi-cache
export POLKIT_POLICY_DIR=$HALD_TMPDIR/share/PolicyKit/policy

#valgrind --num-callers=20 --show-reachable=yes --leak-check=yes --tool=memcheck ./hald --daemon=no --verbose=yes $@
valgrind --show-reachable=yes --tool=memcheck --leak-check=full --leak-resolution=high --log-file=valgrind.log \
	 ./hald --daemon=no --verbose=yes --exit-after-probing $@
