#!/bin/sh
rm -rf .local-fdi-test
mkdir .local-fdi-test
make -C ../fdi install DESTDIR=`pwd`/.local-fdi-test prefix=/

HAL_FDI_SOURCE_PREPROBE=.local-fdi-test/share/hal/fdi/preprobe
HAL_FDI_SOURCE_INFORMATION=.local-fdi-test/share/hal/fdi/preprobe
HAL_FDI_SOURCE_POLICY=.local-fdi-test/share/hal/fdi/policy
HAL_FDI_CACHE_NAME=.local-fdi-test/hald-local-fdi-cache
export HAL_FDI_SOURCE_PREPROBE HAL_FDI_SOURCE_INFORMATION \
       HAL_FDI_SOURCE_POLICY HAL_FDI_CACHE_NAME

#gdb run --args ./hald-generate-fdi-cache
./hald-generate-fdi-cache || exit 2
./hald-cache-test || exit 2

#required by distcheck
rm -Rf .local-fdi-test
