/*
 * Chunkfs pool summary definitions.
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

/*
 * A wee little summary of the whole pool.  There should be one copy
 * of this summary every device.
 *
 * There is no size or block summary in this structure.  Information
 * about the usable size of the file system is only known by the
 * client file systems inside each chunk.  We have to query them
 * individually to find out this information.  Currently, I see no
 * compelling reason to store a summary on disk - it only allows for
 * it to be out of sync with the real accounting information.
 */

/* Pool (superblock) magic number goes in linux/magic.h */
#include <linux/magic.h>

struct chunkfs_pool {
	__le32 p_magic;
	__le32 p_chksum;
	__le64 p_flags;
	struct chunkfs_dev_desc p_root_desc;	/* Device containing root */
};

/*
 * Offset from beginning of partition of the pool summary/superblock.
 * A large initial offset avoids MBR, boot blocks, etc.
 *
 * XXX Any problems from picking this location?
 */

#define	CHUNKFS_POOL_BLK	8
#define	CHUNKFS_POOL_OFFSET	(CHUNKFS_POOL_BLK * CHUNKFS_BLK_SIZE)

static inline int check_pool(struct chunkfs_pool *pool)
{
	return check_metadata(pool, sizeof(*pool), CHUNKFS_SUPER_MAGIC);
}

#ifdef __KERNEL__

#include <linux/buffer_head.h>

struct chunkfs_pool_info {
	struct list_head pi_dlist_head; /* List of devices in this pool */
	struct chunkfs_dev_info *pi_root_dev;
	struct buffer_head *pi_bh;
	/* Use bytes instead of blocks - block size may vary */
	/*
	 * Note that with shared storage or dynamically allocated
	 * inodes, you don't want to assume that total = used + free
	 */
	__u64 pi_bytes_total;
	__u64 pi_bytes_free;
	__u64 pi_bytes_used;
	__u64 pi_inodes_total;
	__u64 pi_inodes_free;
	__u64 pi_inodes_used;
	__u64 pi_flags;
};

static inline struct chunkfs_pool_info * CHUNKFS_PI(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct chunkfs_pool * CHUNKFS_POOL(struct chunkfs_pool_info *pi)
{
	return (struct chunkfs_pool *) pi->pi_bh->b_data;
}

#endif /* __KERNEL__ */
