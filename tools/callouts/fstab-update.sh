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

if test "$HAL_PROP_BLOCK_NO_PARTITIONS" != "true"; then
    if test "$HAL_PROP_BLOCK_IS_VOLUME" != "true"; then
	echo "not a volume"
	exit 0
    fi
else
    if test "$HAL_PROP_BLOCK_IS_VOLUME" = "true"; then
	echo "volume, but on a block.no_partition device"
	exit 0
    fi
fi

# NOTE: We could use HAL_PROP_BLOCK_VOLUME_LABEL (which may or may not be 
# available) but that would be a bad idea since it won't work for two
# volumes with the same label
MEDIAROOT="/media"
if [ -d /mnt -a ! -d /media ]; then
    MEDIAROOT="/mnt"
fi

MOUNTPOINT="$MEDIAROOT/hal/disk-$HAL_PROP_BLOCK_MAJOR-$HAL_PROP_BLOCK_MINOR-"

have_lock=false
max_loops=10
loop_times=0

while [ $have_lock = false -a $loop_times -lt $max_loops ]; do
    loop_times=$((loop_times+1))

    if [ -n /etc/fstab-lock ]; then
	echo "$$" >> /etc/fstab-lock
    fi

    if [ "`head -n 1 /etc/fstab-lock`" = "$$" ]; then
	have_lock=true
    else
	echo "waiting for fstab lock... ($HAL_PROP_BLOCK_DEVICE: $loop_times of $max_loops)"
	sleep 1
    fi
done

# Took too long!
if [ $loop_times -eq $max_loops ]; then
    echo "couldn't get lock after $max_loops seconds.  bailing out!"
    exit 1
fi

if test "$1" = "add"; then

    if [ ! -d $MOUNTPOINT ]; then
	mkdir -p $MOUNTPOINT
    fi

    # Add the device to fstab if it's not already there.
    grep "^$HAL_PROP_BLOCK_DEVICE" /etc/fstab > /dev/null
    if [ $? -ne 0 ]; then
        cp /etc/fstab /etc/fstab-hal
	echo -ne "$HAL_PROP_BLOCK_DEVICE\t" >> /etc/fstab-hal
	echo -ne "$MOUNTPOINT\t" >> /etc/fstab-hal
        # HAL might have autodetected the filesystem type for us - in that
        # case use it...
	if test $HAL_PROP_VOLUME_FSTYPE; then
	    if test $HAL_PROP_VOLUME_FSTYPE = "msdos"; then
		echo -ne "vfat\t" >> /etc/fstab-hal
	    else
		echo -ne "$HAL_PROP_VOLUME_FSTYPE\t" >> /etc/fstab-hal
	    fi
	else
	    echo -ne "auto\t" >> /etc/fstab-hal
	fi
	echo -e  "noauto,user,exec 0 0" >> /etc/fstab-hal

        # Make sure it's here
        if [ -f /etc/fstab-hal -a -s /etc/fstab-hal ]; then
            mv -f /etc/fstab-hal /etc/fstab
        fi
    fi

elif test "$1" = "remove"; then
    grep -v "$MOUNTPOINT" /etc/fstab > /etc/fstab-hal

    # Make sure it's here
    if [ -f /etc/fstab-hal -a -s /etc/fstab-hal ]; then
	mv -f /etc/fstab-hal /etc/fstab
    fi

    if [ -d $MOUNTPOINT ]; then
	rmdir $MOUNTPOINT
    fi

else
    echo "invalid action!"
fi

rm -f /etc/fstab-lock
