#!/bin/sh

export PATH=linux2/probing:$PATH
./hald --daemon=no --verbose=yes
