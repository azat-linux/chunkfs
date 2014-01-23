#!/bin/bash -x
#
# Give me a naked eyeball look at the on-disk format as created by mkfs.
#

# Name of the loopback device we're creating
DEV=/tmp/disk0
# This is where the chunkfs user binaries are located.
BINPATH=.

${BINPATH}/write_pattern ${DEV}
if [ "$?" != "0" ]; then
    echo "write_pattern failed"
    exit 1
fi

${BINPATH}/mkfs.chunkfs ${DEV}
if [ "$?" != "0" ]; then
    echo "mkfs.chunkfs failed"
    exit 1
fi

# Now dump it for us.

od -A d -t x4 ${DEV}

exit 0
