#!/bin/bash
#
# This shell script updates your /etc/fstab when volumes when they
# appear and disappear from HAL.
#
# ** WARNING **
# This file is for informational purposes only and as an example
# of how one might write a HAL callout script.  There are no
# guarantees for safety of this script.  If you want to try it
# out, you should make a backup of your existing /etc/fstab
# file, and you shouldn't use this on any production system!
#
# If you are running SUSE (or another FHS 2.3-compliant OS)
# you should probably change the MEDIAROOT from "/mnt" to
# "/media"

if test "$HAL_PROP_BLOCK_DEVICE" = ""; then
    exit 0
fi

if test "$HAL_PROP_BLOCK_MAJOR" = ""; then
    echo "no device major number"
    exit 0
fi

if test "$HAL_PROP_BLOCK_MINOR" = ""; then
    echo "no device minor number"
    exit 0
fi

if test "$HAL_PROP_BLOCK_IS_VOLUME" != "true"; then
    echo "not a volume"
    exit 0
fi

MEDIAROOT="/mnt"
MOUNTPOINT="$MEDIAROOT/hal/disk-$HAL_PROP_BLOCK_MAJOR-$HAL_PROP_BLOCK_MINOR"

if test "$1" = "add"; then

    if [ ! -d $MOUNTPOINT ]; then
	mkdir -p $MOUNTPOINT
    fi

    # Add the device to fstab if it's not already there.
    grep "^$HAL_PROP_BLOCK_DEVICE" /etc/fstab > /dev/null
    if [ $? -ne 0 ]; then
	echo -ne "$HAL_PROP_BLOCK_DEVICE\t" >> /etc/fstab
	echo -ne "$MOUNTPOINT\t" >> /etc/fstab
	echo -ne "auto\t" >> /etc/fstab
	echo -e  "noauto,user,exec 0 0" >> /etc/fstab
    fi

elif test "$1" = "remove"; then
    grep -v "$MOUNTPOINT" /etc/fstab > /etc/fstab-tmp
    mv -f /etc/fstab-tmp /etc/fstab

    if [ -d $MOUNTPOINT ]; then
	rmdir $MOUNTPOINT
    fi

else
    echo "invalid action!"
fi
