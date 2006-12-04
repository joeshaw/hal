#!/bin/sh

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
    make -C ../fdi install DESTDIR=`pwd`/.local-fdi prefix=/
fi
export HAL_FDI_SOURCE_PREPROBE=.local-fdi/share/hal/fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=.local-fdi/share/hal/fdi/information
export HAL_FDI_SOURCE_POLICY=.local-fdi/share/hal/fdi/policy

#valgrind --num-callers=20 --show-reachable=yes --leak-check=yes --tool=memcheck ./hald --daemon=no --verbose=yes $@
valgrind --show-reachable=yes --tool=memcheck --leak-check=full ./hald --daemon=no --verbose=yes $@
