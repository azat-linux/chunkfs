#
# Makefile for chunkfs.
#

obj-$(CONFIG_CHUNK_FS) += chunkfs.o

chunkfs-y	:= super.o inode.o dir.o file.o namei.o symlink.o cont.o

#
# Temporarily keep utilities in this dir too.
#
HOST_EXTRACFLAGS += -I$(src)/../../include -static

hostprogs-$(CONFIG_CHUNK_FS)	:= mkfs.chunkfs write_pattern

always          := $(hostprogs-y) $(hostprogs-m)
