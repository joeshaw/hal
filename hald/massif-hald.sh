#!/bin/sh

information_fdidir="../../hal-info/fdi"

case `uname -s` in
    FreeBSD)    backend=freebsd ;;
    SunOS)      backend=solaris ;;
    *)          backend=linux ;;
esac
export HALD_RUNNER_PATH=`pwd`/$backend:`pwd`/$backend/probing:`pwd`/$backend/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/$backend
export PATH=`pwd`/../hald-runner:$PATH

HALD_TMPDIR=/tmp/run-hald-$USER

if [ "$1" = "--skip-fdi-install" ] ; then
    shift
else
    rm -rf $HALD_TMPDIR
    mkdir -p $HALD_TMPDIR
    make -C ../policy install DESTDIR=$HALD_TMPDIR prefix=/
    make -C ../fdi install DESTDIR=$HALD_TMPDIR prefix=/ && \
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

#delete all old memory outputs, else we get hundreds
rm massif.*

#G_SLICE="always-malloc" valgrind --num-callers=24 --tool=massif --depth=24 --format=html \
G_SLICE="always-malloc" valgrind --num-callers=24 --tool=massif --depth=24  \
        --alloc-fn=g_malloc --alloc-fn=g_realloc \
	--alloc-fn=g_try_malloc --alloc-fn=g_malloc0 --alloc-fn=g_mem_chunk_all \
        --alloc-fn=g_slice_alloc0 --alloc-fn=g_slice_alloc \
	--alloc-fn=dbus_realloc \
	 ./hald --daemon=no --verbose=yes --retain-privileges --exit-after-probing

#massif uses the pid file, which is hard to process.
mv massif.*.html massif.html
mv massif.*.ps massif.ps
#convert to pdf, and make readable by normal users
ps2pdf massif.ps massif.pdf
chmod a+r massif.*
