#!/bin/bash -x
#
# Run simple cross-chunk checks.
#

mount /dev/loop1 /chunk1
mount /dev/loop2 /chunk2
mount /dev/loop3 /chunk3

# For each file in the continued-from-directory, check to see if it
# exists in the original chunk.

# For every possible continuation file...

for from_chunk in 1 2 3; do
    for to_chunk in 1 2 3; do
	for i in `ls /chunk${to_chunk}/${from_chunk}/*` ; do
	    # Check to see if the previous inode exists
	    echo testi \<${i}\> | debugfs /dev/loop${from_chunk} | grep marked
	    # Returns 1 if not allocated (and it should be)
	    if [ "$?" == "1" ]; then
		echo "Orphan continuation ${i}, removing"
		rm ${i}
	    fi
	done
    done
done

umount /dev/loop1
umount /dev/loop2
umount /dev/loop3

exit 0
