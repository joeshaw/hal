#!/bin/sh

export PATH=linux2:linux2/probing:linux2/addons:.:../tools:$PATH
export HAL_FDI_SOURCE=../fdi
./hald --daemon=no --verbose=yes --retain-privileges
#./hald --daemon=no --retain-privileges

