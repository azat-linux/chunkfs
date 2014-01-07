#                                     #
# Build separately from linux kernel. #
#                                     #
kdir := $(shell readlink -f /lib/modules/$(shell uname -r)/source)
ifneq ($(kdir),$(PWD))
	CONFIG_CHUNK_FS := m
endif

#                                           #
# Makefile for chunkfs inside linux kernel. #
#                                           #
obj-$(CONFIG_CHUNK_FS) += chunkfs.o
chunkfs-y := super.o inode.o dir.o file.o namei.o symlink.o cont.o

#
# Temporarily keep utilities in this dir too.
#
HOST_EXTRACFLAGS += -I$(src)/../../include -static
hostprogs-$(CONFIG_CHUNK_FS) := mkfs.chunkfs write_pattern
always := $(hostprogs-y) $(hostprogs-m)

#                                     #
# Build separately from linux kernel. #
#                                     #
all: $(hostprogs-$(CONFIG_CHUNK_FS)) ko
# Not a real path, just to make $(HOST_EXTRACFLAGS) work
src := $(kdir)/fs/chunkfs

mkfs.chunkfs:
	$(CC) $(HOST_EXTRACFLAGS) -o $@ $@.c

ko:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
