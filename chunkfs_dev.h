/*
 * Chunkfs device definitions
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

/*
 * Device summary.  This contains:
 *
 * - Information about which part of the device we manage
 * - Pointer to the first chunk header (root chunk is flagged)
 *
 * Again, free/used information is known only by chunks, so we do not
 * keep summary info in the dev summary unless we find some
 * performance reason to keep it on disk.
 */

#define	CHUNKFS_DEV_MAGIC	0xdeeddeed

struct chunkfs_dev {
	__le32 d_magic;
	__le32 d_chksum;
	__le64 d_flags;		/* Clean unmounted, etc. */
	__le64 d_uuid;
	c_byte_t d_begin;	/* Total space we manage */
	c_byte_t d_end;
	c_byte_t d_innards_begin; /* Space for chunks */
	c_byte_t d_innards_end;
	c_byte_t d_root_chunk;	/* Offset of chunk containing root, if here */
	struct chunkfs_dev_desc d_next_dev; /* Next device in pool */
};

/*
 * Dev flags
 */

#define	CHUNKFS_ROOT_DEV		0x00000001ULL

#define CHUNKFS_IS_ROOT_DEV(ci)	(ci->ci_flags & CHUNKFS_ROOT_DEV)

#define	CHUNKFS_DEV_BLK		(CHUNKFS_POOL_BLK + 1)
#define	CHUNKFS_DEV_OFFSET	(CHUNKFS_DEV_BLK * CHUNKFS_BLK_SIZE)

static inline int check_dev(struct chunkfs_dev *dev)
{
	return check_metadata(dev, sizeof(*dev), CHUNKFS_DEV_MAGIC);
}

#ifdef __KERNEL__

struct chunkfs_dev_info {
	struct chunkfs_pool_info *di_pool;
	struct list_head di_dlist;	/* Member of list of devs */
	struct list_head di_clist_head;	/* Pointer to list of chunks */
	struct chunkfs_chunk_info *di_root_chunk;
	struct buffer_head *di_bh;
	__u64 di_flags;
	/* The rest of the on-disk data is not normally used. */
};

static inline struct chunkfs_dev * CHUNKFS_DEV(struct chunkfs_dev_info *di)
{
	return (struct chunkfs_dev *) di->di_bh->b_data;
}

#endif /* __KERNEL__ */
