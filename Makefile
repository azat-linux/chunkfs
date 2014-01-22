obj-m += chunkfs.o
chunkfs-y := super.o inode.o dir.o file.o namei.o symlink.o cont.o
hostprogs-y := mkfs.chunkfs write_pattern

all: $(hostprogs-y) ko

ko:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
