#!/bin/bash -x
#
# Start UML.  The real test script is run from inside the UML
# partition; I suggest using hostfs to mount it.
#
ROOT=/home/val/root_fs_philips

# Set up some gdb commands

cat > /tmp/gdb_commands <<EOF
set args ubd0=${ROOT}
handle SIGUSR1 nostop noprint
run
EOF

gdb -x /tmp/gdb_commands ./linux

# We may have exited improperly and left UML threads running, kill 'em
# XXX This is dangerous and stupid

pkill linux
