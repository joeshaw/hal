#!/bin/sh

export HALD_RUNNER_PATH=`pwd`/linux2:`pwd`/linux2/probing:`pwd`/linux2/addons:`pwd`/.:`pwd`/../tools:`pwd`/../tools/linux
export PATH=`pwd`/../hald-runner:$PATH
export HAL_FDI_SOURCE_PREPROBE=../fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=../fdi/information
export HAL_FDI_SOURCE_POLICY=../fdi/policy
./hald --daemon=no --verbose=yes
#./hald --daemon=no


