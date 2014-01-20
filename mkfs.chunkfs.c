/*
 * Create a chunkfs file system.
 *
 * (C) 2007-2008 Val Henson <val@nmt.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <asm/byteorder.h>

#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

/* Compile time test that structures have not outgrown blocks. */

static char canary_buf1[CHUNKFS_BLK_SIZE -
			sizeof(struct chunkfs_pool)] __attribute__((unused));
static char canary_buf2[CHUNKFS_BLK_SIZE -
			sizeof(struct chunkfs_dev)] __attribute__((unused));

static char * cmd;

static void usage (void)
{
	fprintf(stderr, "Usage: %s <device>\n", cmd);
	exit(1);
}

static void write_block(void *metadata, int size, int fd, __u64 offset)
{
	char buf[CHUNKFS_BLK_SIZE];
	int buf_size = sizeof (buf);
	struct chunkfs_chkmagic *x = (struct chunkfs_chkmagic *) buf;

	bzero(buf, buf_size);
	memcpy(buf, metadata, size);
	write_chksum(buf, size);

	printf("Writing magic %0x chksum %0x to offset %llu\n",
	       __le32_to_cpu(x->x_magic), __le32_to_cpu(x->x_chksum),
	       offset);

	if (lseek(fd, offset, SEEK_SET) < 0)
		error(1, errno, "Cannot seek");

	if (write(fd, buf, buf_size) < buf_size)
		error(1, errno, "Cannot write metadata at offset %llu",
		      (unsigned long long) offset);
}

/*
 * Create and write a pool summary (superblock)
 */
static void create_pool_summary(char *dev_name, struct chunkfs_pool *pool)
{
	struct chunkfs_dev_desc *dev_desc = &pool->p_root_desc;

	/* Fill in device description. */
	strcpy(dev_desc->d_hint, dev_name);
	/* XXX need userland generated uuid  */
	dev_desc->d_uuid = __cpu_to_le64(0x001d001d);

	bzero(pool, sizeof(*pool));
	pool->p_magic = __cpu_to_le32(CHUNKFS_SUPER_MAGIC);
}

static void create_dev_summary(struct chunkfs_pool *pool,
			       struct chunkfs_dev *dev,
			       __u64 dev_begin,
			       __u64 dev_size)
{
	struct chunkfs_dev_desc *dev_desc = &pool->p_root_desc;

	bzero(dev, sizeof(*dev));
	dev->d_uuid = dev_desc->d_uuid; /* Already swapped */
	dev->d_begin = __cpu_to_le64(dev_begin);
	dev->d_end = __cpu_to_le64(dev_begin + dev_size - 1); /* Starting counting from zero */
	dev->d_innards_begin = __cpu_to_le64(dev_begin + CHUNKFS_BLK_SIZE);
	dev->d_innards_end = dev->d_end; /* Already swapped */
	dev->d_root_chunk = dev->d_innards_begin; /* Already swapped */
	dev->d_magic = __cpu_to_le32(CHUNKFS_DEV_MAGIC);
}

static void create_chunk_summary(struct chunkfs_chunk *chunk,
				 __u64 chunk_start, __u64 chunk_size,
				 __u64 chunk_id)
{
	bzero(chunk, sizeof(*chunk));
	chunk->c_begin = __cpu_to_le64(chunk_start);
	chunk->c_end = __cpu_to_le64(chunk_start + chunk_size - 1);
	chunk->c_innards_begin = __cpu_to_le64(chunk_start + CHUNKFS_BLK_SIZE);
	chunk->c_innards_end = chunk->c_end; /* Already swapped */
	chunk->c_chunk_id = __cpu_to_le64(chunk_id);
	chunk->c_magic = __cpu_to_le32(CHUNKFS_CHUNK_MAGIC);
}

static void write_chunk_summaries(struct chunkfs_dev *dev,
				  struct chunkfs_chunk *chunk,
				  int fd)
{
	__u64 chunk_id = 1; /* 0 is not a valid chunk id */
	__u64 chunk_start = __le64_to_cpu(dev->d_root_chunk);
	__u64 chunk_size = CHUNKFS_CHUNK_SIZE;
	__u64 dev_end = __le64_to_cpu(dev->d_end);

	while ((chunk_start + chunk_size - 1) < dev_end) {
		/* XXX Throwing away disk if not multiple of chunk size */
		create_chunk_summary(chunk, chunk_start, chunk_size,
				     chunk_id);
		if (chunk_id == 1)
			chunk->c_flags |= __cpu_to_le64(CHUNKFS_ROOT);
		/* Can we get another chunk in? Then point to it */
		if ((__le64_to_cpu(chunk->c_end) + chunk_size - 1) < dev_end)
			chunk->c_next_chunk = __cpu_to_le64(chunk->c_end + 1);

		printf("Writing chunk %llu: start %llu end %llu)\n",
		       __le64_to_cpu(chunk->c_chunk_id),
		       __le64_to_cpu(chunk->c_begin),
		       __le64_to_cpu(chunk->c_end));

		printf("clientfs: start %llu\n", __le64_to_cpu(chunk->c_innards_begin));

		write_block(chunk, sizeof(*chunk), fd, chunk_start);
		chunk_start += chunk_size;
		chunk_id++;
	}
}

int main (int argc, char * argv[])
{
	int fd;
	char * dev_name;
	struct stat stat_buf;
	off_t raw_dev_size;
	struct chunkfs_pool pool = { 0 };
	struct chunkfs_dev root_dev = { 0 };
	struct chunkfs_chunk root_chunk = { 0 };

	cmd = argv[0];

	if (argc != 2)
		usage();

	dev_name = argv[1];

	/*
	 * Get some info about the device.
	 */

	if ((fd = open(dev_name, O_RDWR)) < 0)
		error(1, errno, "Cannot open device %s", dev_name);

	if (fstat(fd, &stat_buf) != 0)
		error(1, errno, "Cannot stat device %s", dev_name);
	raw_dev_size = stat_buf.st_size;
	/*
	 * XXX Sanity check size - big enough?
	 */

	/*
	 * Create structures and write them out
	 */

	create_pool_summary(dev_name, &pool);
	write_block(&pool, sizeof(pool), fd, CHUNKFS_POOL_OFFSET);

	/* XXX handle multiple devs */

	create_dev_summary(&pool, &root_dev, CHUNKFS_DEV_OFFSET,
			   raw_dev_size - CHUNKFS_DEV_OFFSET - 1);
	write_block(&root_dev, sizeof(root_dev), fd, CHUNKFS_DEV_OFFSET);

	/* Now we get to the meaty bit: chunk summaries. */

	write_chunk_summaries(&root_dev, &root_chunk, fd);

	close(fd);

	return 0;
}
