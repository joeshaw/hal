#!/bin/sh

export PATH=linux2:linux2/probing:linux2/addons:.:../tools:$PATH
export HAL_FDI_SOURCE=../fdi
gdb run --args ./hald --daemon=no --verbose=yes --retain-privileges

