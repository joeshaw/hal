#!/bin/sh

export PATH=linux2:linux2/probing:linux2/addons:.:../tools:../tools/linux:$PATH
export HAL_FDI_SOURCE_PREPROBE=../fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=../fdi/information
export HAL_FDI_SOURCE_POLICY=../fdi/policy
./hald --daemon=no --verbose=yes --retain-privileges
#./hald --daemon=no --retain-privileges

