#!/bin/sh

export PATH=linux2:linux2/probing:linux2/addons:.:../tools:$PATH
export HAL_FDI_SOURCE=../fdi
echo ========================================
echo Just type \'run\' to start debugging hald
echo ========================================
gdb run --args ./hald --daemon=no --verbose=yes --retain-privileges

