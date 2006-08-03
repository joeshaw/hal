#!/bin/sh
watch -n 0.5 "clear; mount; echo; ls -l /media; echo; cat /media/.hal-mtab; echo; ls -l /dev/mapper/; echo; tree /dev/disk/by-uuid echo"
