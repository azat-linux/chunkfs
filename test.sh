#!/bin/bash -x
#
# Test script for chunkfs, to be run from inside UML.
#

function getSelfDirectory()
{
    self="${1%/*}"
    if [ ! ${self:0:1} = "/" ]; then
        self="$PWD/$self"
    fi

    echo "$self/"
}
SELF="$(getSelfDirectory $0)"

# Name of the file backing the loop device
FILE=/loop/disk0
# This is where the chunkfs user binaries are located.
BINPATH="$SELF"
MNT=/mnt

# Clean up from last iteration if necessary.

MOUNTED=`cat /proc/mounts | grep "${DEV} ${MNT} chunkfs"`
if [ -n "$MOUNTED" ]; then
    umount ${MNT}
    if [ "$?" != "0" ]; then
	echo "umount failed"
    exit 1
    fi
fi

# Tear down old mounts and loop devices
loop_num=$((0))
while (losetup /dev/loop$loop_num); do
    umount /chunk${loop_num}
    losetup -d /dev/loop$loop_num
    loop_num=$((loop_num + 1))
done

${BINPATH}/write_pattern ${FILE}
if [ "$?" != "0" ]; then
    echo "write_pattern failed"
    exit 1
fi

${BINPATH}/mkfs.chunkfs ${FILE} > /tmp/offsetlist
if [ "$?" != "0" ]; then
    echo "mkfs.chunkfs failed"
    exit 1
fi

# Create primary loop device
losetup /dev/loop0 ${FILE}
if [ "$?" != "0" ]; then
    echo "Create loop device failed"
    exit 1
fi

# XXX Wow, like, such a hack.  Set up a bunch of block devices
# starting at different offsets in order to create ext2 file systems
# inside chunks.  losetup doesn't include an end argument, so there's
# no protection against one file system having a bug that scribbles
# over the following file systems.  Also, mkfs should do this
# directly.
#
# XXX More hackery.  Mount all our client fs's so that chunkfs kernel
# side can lookup the path and grab the superblocks.

OFFSETS="`awk '/clientfs: start/ {print $3}' /tmp/offsetlist`"
loop_num=$((1))
for offset in ${OFFSETS}; do
    losetup -o $offset /dev/loop$loop_num ${FILE}
    mke2fs -b 4096 /dev/loop$loop_num 2559 > /dev/null
    mkdir -p /chunk${loop_num}
    mount -t ext2 -o user_xattr /dev/loop${loop_num} /chunk${loop_num}
    if [ "$?" != "0" ]; then
	echo "mount client fs failed"
	exit 1
    fi
    # Only the root chunk has the root directory
    if [ "$loop_num" == "1" ]; then
	mkdir -p /chunk${loop_num}/root
	# Create continuation data
	# Hackity hack.  Just create it on /root if it doesn't already
	# exist.
	/usr/local/usr/bin/setfattr -n user.next -v 0 /chunk${loop_num}/root
	/usr/local/usr/bin/setfattr -n user.prev -v 0 /chunk${loop_num}/root
	/usr/local/usr/bin/setfattr -n user.start -v 0 /chunk${loop_num}/root
	/usr/local/usr/bin/setfattr -n user.len -v 40960 /chunk${loop_num}/root
    fi
    for i in 0 1 2 3 4 5 6 7 8 9 private; do
	mkdir -p /chunk${loop_num}/$i
    done
    loop_num=$((loop_num + 1))
done

${BINPATH}/mount_chunkfs /dev/loop0 ${MNT}
if [ "$?" != "0" ]; then
    echo "mount_chunkfs failed"
    exit 1
fi

# Now test a few more things

ls /mnt
touch /mnt/a_file
touch /mnt/another_file
echo "file data" > /mnt/a_file_with_data
cat /mnt/a_file_with_data
rm /mnt/another_file
mkdir /mnt/a_dir
mkdir /mnt/a_dir/a_dir
mkdir /mnt/a_dir/another_dir
rmdir /mnt/a_dir/another_dir
ln -s /mnt/a_file /mnt/a_symlink
ls -l /mnt/a_symlink
cat /mnt/a_symlink
dd if=/dev/zero of=/mnt/big bs=4096 count=11
ls -l /mnt/big
ls -l /chunk1/root/big
ls -l /chunk2/1/29

exit 0
