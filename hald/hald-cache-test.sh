#!/bin/sh
rm -rf .local-fdi
make -C ../fdi install DESTDIR=`pwd`/.local-fdi prefix=/

HAL_FDI_SOURCE_PREPROBE=.local-fdi/share/hal/fdi/preprobe
HAL_FDI_SOURCE_INFORMATION=.local-fdi/share/hal/fdi/information
HAL_FDI_SOURCE_POLICY=.local-fdi/share/hal/fdi/policy
HAL_FDI_CACHE_NAME=.local-fdi/hald-local-fdi-cache
export HAL_FDI_SOURCE_PREPROBE HAL_FDI_SOURCE_INFORMATION \
       HAL_FDI_SOURCE_POLICY HAL_FDI_CACHE_NAME

./hald-generate-fdi-cache || exit 2
./hald-cache-test || exit 2
