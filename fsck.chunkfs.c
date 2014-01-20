/*
 * Check and repair a chunkfs file system.
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

static char * cmd;

static void usage (void)
{
	fprintf(stderr, "Usage: %s <device>\n", cmd);
	exit(1);
}

static void read_data(void *buf, int size, int fd, size_t offset)
{
	bzero(buf, size);

	if (lseek(fd, block, SEEK_SET) < 0)
		error(1, errno, "Cannot seek");

	if (read(fd, buf, size) < size)
		error(1, errno, "Cannot read
}

/*
 * Construct a "superblock."  In chunkfs, this means a pool summary.
 */
static void create_pool_summary(char *dev_name, struct chunkfs_pool *pool)
{
	struct chunkfs_dev_desc *root_dev = &pool->p_root;
	int hint_len = sizeof (root_dev->d_hint);

	bzero(pool, sizeof(*pool));
	pool->p_magic = __cpu_to_le32(CHUNKFS_SUPER_MAGIC);
	/* Fill in root device description. */
	strncpy(root_dev->d_hint, dev_name, hint_len);
	root_dev->d_hint[hint_len - 1] = '\0';
	pool->p_flags = 0;
	/* XXX need userland generated uuid... ask kernel to do it on mount? */
	root_dev->d_uuid = __cpu_to_le64(0x001d001d);

	write_chksum(pool, sizeof(*pool), &pool->p_chksum);
}

static void create_dev_summary(char *dev_name, int devfd,
			       struct chunkfs_pool *pool,
			       struct chunkfs_dev *dev)
{
	struct stat stat_buf;
	struct chunkfs_dev_desc *root_dev = &pool->p_root;

	if (fstat(devfd, &stat_buf) != 0)
		error(1, errno, "Cannot stat device %s", dev_name);

	bzero(dev, sizeof(*dev));
	dev->d_uuid = root_dev->d_uuid; /* Already endian swapped */
	dev->d_bytes_total = __cpu_to_le64(stat_buf.st_size);
	dev->d_magic = __cpu_to_le32(CHUNKFS_DEV_MAGIC);
	write_chksum(dev, sizeof (dev), &dev->d_chksum);
}

static void create_chunk_summary(struct chunkfs_pool *pool,
				 struct chunkfs_dev *dev,
				 struct chunkfs_chunk *chunk,
				 __u64 start, __u64 size, __u64 next_chunk)
{
	__u64 end = start + size - 1;
	/* XXX use chunk->ci_blk_bits; */
	__u64 start_blk = (start >> CHUNKFS_BLK_BITS) + 2;
	__u64 end_blk = end >> CHUNKFS_BLK_BITS;
	__u64 start_inode;
	__u64 end_inode;

	bzero(chunk, sizeof(*chunk));
	chunk->c_next_chunk = __cpu_to_le64(next_chunk);
	chunk->c_blk_size = __cpu_to_le64(CHUNKFS_BLK_SIZE);
	chunk->c_blk_bits = __cpu_to_le64(CHUNKFS_BLK_BITS);
	chunk->c_blk_start = __cpu_to_le64(start_blk);
	chunk->c_blk_end = __cpu_to_le64(end_blk);
	/* XXX subtract space used for bitmaps and chunk summary
	 * For now assume only takes two blocks
	 * XXX put data at one end of the chunk and metadata at other */
	chunk->c_blks_free = __cpu_to_le64((end_blk - start_blk) - 2);
	start_inode = blk_to_inode(start_blk);
	chunk->c_inode_begin = __cpu_to_le64(start_inode);
	/* Get the last inode in the last blk, not the first */
	end_inode = blk_to_inode(end_blk + 1) - 1;
	chunk->c_inode_end = __cpu_to_le64(end_inode);
	chunk->c_magic = __cpu_to_le32(CHUNKFS_CHUNK_MAGIC);
	write_chksum(chunk, sizeof (chunk), &chunk->c_chksum);
}

static void write_chunk_summaries(int fd, struct chunkfs_pool *pool,
				   struct chunkfs_dev *dev,
				   struct chunkfs_chunk *root_chunk)
{
	struct chunkfs_chunk chunk;
	__u64 dev_size = __le64_to_cpu(dev->d_bytes_total);
	__u64 chunk_size = CHUNKFS_CHUNK_SIZE;
	__u64 chunk_start = CHUNKFS_CHUNK_OFFSET;;
	__u64 next_chunk_offset;
	unsigned int chunk_id = 0;
	int root = 1;

	while (chunk_start < dev_size) {
		/* XXX What is chunk min size? */
		if ((chunk_start + chunk_size) > dev_size) {
			chunk_size = dev_size - chunk_start;
			next_chunk_offset = 0;
		} else {
			next_chunk_offset = chunk_start + chunk_size;
		}
		create_chunk_summary(pool, dev, &chunk, chunk_start,
				     chunk_size, next_chunk_offset);
		if (root) {
			/* Make root the first inode */
			chunk.c_root_inode = chunk.c_inode_begin;
			*root_chunk = chunk;
			root = 0;
		}

		printf("Writing chunk %d (bytes %llu-%llu, blocks %llu-%llu, "
		       "inodes %llu-%llu)\n", chunk.c_id, chunk.c_start,
		       chunk_end, start_blk, end_blk,start_inode, end_inode);

		write_block(chunk, sizeof(*chunk), fd, chunk_start);
		chunk_start += chunk_size;
		*chunk_id++;
	}
}

static void write_root(int fd, struct chunkfs_pool *pool,
		       struct chunkfs_dev *dev,
		       struct chunkfs_chunk *root_chunk,
		       struct chunkfs_inode *root_inode)
{
	/* XXX This is zero presently. */
	c_inode_num_t inode_num = __le64_to_cpu(root_chunk->c_root_inode);
	c_byte_t root_offset = inode_num;

	printf("root inode number %llu, offset %llu\n", inode_num, root_offset);
	root_inode->i_self = __cpu_to_le64(inode_num);
	/* uid, gid, size, etc. all 0 */
	root_inode->i_nlink = __cpu_to_le32(2);
	root_inode->i_atime.t_sec =
		root_inode->i_mtime.t_sec =
		root_inode->i_ctime.t_sec = __cpu_to_le32(time(NULL));
	root_inode->i_mode = __cpu_to_le16(S_IFDIR | 0755); /* XXX */
	root_inode->i_type = __cpu_to_le16(CHUNKFS_PUBLIC_INODE);
	root_inode->i_magic  = __cpu_to_le32(CHUNKFS_INODE_MAGIC);
	write_chksum(root_inode, sizeof(*root_inode), &root_inode->i_chksum);

	write_block(root_inode, sizeof(*root_inode), fd, root_offset);
}

int main (int argc, char * argv[])
{
	int fd;
	char * dev_name;
	struct chunkfs_pool pool = { 0 };
	struct chunkfs_dev root_dev = { 0 };
	struct chunkfs_chunk root_chunk = { 0 };
	struct chunkfs_inode root_inode = { 0 };

	cmd = argv[0];

	if (argc != 2)
		usage();

	dev_name = argv[1];

	if ((fd = open(dev_name, O_RDWR)) < 0)
		error(1, errno, "Cannot open device %s", dev_name);
	/* XXX combine create and write_block */
	/* The chunkfs equivalent of a superblock is the pool summary. */

	create_pool_summary(dev_name, &pool);
	write_block(&pool, sizeof(pool), fd, CHUNKFS_POOL_OFFSET);

	/* We need one device summary per device. */

	create_dev_summary(dev_name, fd, &pool, &root_dev);
	write_block(&root_dev, sizeof(root_dev), fd, CHUNKFS_DEV_OFFSET);

	/* Now we get to the meaty bit: chunk summaries. */

	write_chunk_summaries(fd, &pool, &root_dev, &root_chunk);

	/* Write root inode */

	write_root(fd, &pool, &root_dev, &root_chunk, &root_inode);

	close(fd);

	return 0;
}
