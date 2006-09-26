#!/bin/sh

export HALD_RUNNER_PATH=`pwd`/linux:`pwd`/linux/probing:`pwd`/linux/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/linux
export PATH=`pwd`/../hald-runner:$PATH

rm -rf .local-fdi
make -C ../fdi install DESTDIR=`pwd`/.local-fdi prefix=/
export HAL_FDI_SOURCE_PREPROBE=.local-fdi/share/hal/fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=.local-fdi/share/hal/fdi/information
export HAL_FDI_SOURCE_POLICY=.local-fdi/share/hal/fdi/policy

./hald --daemon=no --verbose=yes
#./hald --daemon=no


