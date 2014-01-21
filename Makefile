obj-m += chunkfs.o
chunkfs-y := super.o inode.o dir.o file.o namei.o symlink.o cont.o

KDIR := $(shell readlink -f /lib/modules/$(shell uname -r)/source)
HOST_EXTRACFLAGS += -I$(KDIR)/fs/chunkfs/../../include -static

hostprogs-y := mkfs.chunkfs write_pattern


all: $(hostprogs-y) ko

mkfs.chunkfs:
	$(CC) $(HOST_EXTRACFLAGS) -o $@ $@.c

ko:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
