#!/bin/sh

export HALD_RUNNER_PATH=`pwd`/linux:`pwd`/linux/probing:`pwd`/linux/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/linux
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

#delete all old memory outputs, else we get hundreds
rm massif.*

valgrind --tool=massif --format=html --depth=10 \
	 --alloc-fn=g_malloc --alloc-fn=g_realloc \
	 --alloc-fn=g_try_malloc --alloc-fn=g_malloc0 --alloc-fn=g_mem_chunk_alloc \
	 ./hald --daemon=no --verbose=yes --retain-privileges --exit-after-probing

#massif uses the pid file, which is hard to process.
mv massif.*.html massif.html
mv massif.*.ps massif.ps
#convert to pdf, and make readable by normal users
ps2pdf massif.ps massif.pdf
chmod a+r massif.*
