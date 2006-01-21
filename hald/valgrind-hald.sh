#!/bin/sh

export HALD_RUNNER_PATH=linux2:linux2/probing:linux2/addons:.:../tools:../tools/linux
export PATH=../hald-runner:$PATH
export HAL_FDI_SOURCE_PREPROBE=../fdi/preprobe
export HAL_FDI_SOURCE_INFORMATION=../fdi/information
export HAL_FDI_SOURCE_POLICY=../fdi/policy
valgrind --num-callers=20 --show-reachable=yes --leak-check=yes --tool=memcheck ./hald --daemon=no --verbose=yes
