#!/bin/bash

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

MOUNTPOINT="/mnt/hal/disk-$HAL_PROP_BLOCK_MAJOR-$HAL_PROP_BLOCK_MINOR"

if test "$1" = "add"; then

    if [ ! -d /mnt ]; then
        mkdir /mnt
    fi

    if [ ! -d /mnt/hal ]; then
        mkdir /mnt/hal
    fi

    if [ ! -d $MOUNTPOINT ]; then
	mkdir $MOUNTPOINT
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
