#!/bin/bash -x
#
# Demo script for chunkfs to be run from inside UML.
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

# This is where the chunkfs user binaries are located.
BINPATH="$SELF"
MNT=/mnt

# Name of the file backing the loop device
ORIG=/loop/saved_disk
FILE=/loop/test_disk
cp ${ORIG} ${FILE}

# Unmount chunkfs and chunk file systems before stomping
umount ${MNT}
umount /dev/loop1
umount /dev/loop2
umount /dev/loop3

losetup -d /dev/loop0
losetup -d /dev/loop1
losetup -d /dev/loop2
losetup -d /dev/loop3

losetup /dev/loop0 ${FILE}
losetup -o 45056 /dev/loop1 ${FILE}
losetup -o 10530816 /dev/loop2 ${FILE}
losetup -o 21016576 /dev/loop3 ${FILE}

# Stomp head inode

OFFSET=$(((4096 * 4) + 0x0e00))
dd if=/dev/zero of=/dev/loop1 seek=${OFFSET} bs=1 count=128

# Repair individual chunks

fsck -f /dev/loop1
fsck -f /dev/loop2
fsck -f /dev/loop3

# Cross-chunk repair

${BINPATH}/cross.sh

for i in 1 2 3; do
    mount -t ext2 -o user_xattr /dev/loop${i} /chunk${i}
done

${BINPATH}/mount_chunkfs /dev/loop0 ${MNT}

ls /mnt/big
ls /chunk2/1/29

exit 0
