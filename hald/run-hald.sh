#!/bin/sh

export PATH=linux2:linux2/probing:linux2/addons:.:$PATH
./hald --daemon=no --verbose=yes
