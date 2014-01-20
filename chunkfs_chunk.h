/*
 * Chunkfs chunk definitions
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

/*
 * Chunk summary.  This is just a wrapper defining the chunk size and
 * giving us enough information to identify the client file system
 * living inside this chunk and use its routines to mount it.
 *
 * XXX Question: what about df?  Will it require us to df each
 * individual chunk?  Maybe a higher level summary is a good idea.
 *
 */

#define	CHUNKFS_CHUNK_MAGIC	0xf00df00d

/* XXX super_block s_id len is 32, should #define it */

#define	CHUNKFS_CLIENT_NAME_LEN	32

struct chunkfs_chunk {
	__le32 c_magic;
	__le32 c_chksum;
	__le64 c_flags;
	__le64 c_chunk_id;
	c_byte_t c_begin;
	c_byte_t c_end;
	c_byte_t c_innards_begin;
	c_byte_t c_innards_end;
	c_byte_t c_next_chunk;
	char c_client_fs[CHUNKFS_CLIENT_NAME_LEN];
};

#define	CHUNKFS_CHUNK_BLK	(CHUNKFS_DEV_BLK + 1)
#define	CHUNKFS_CHUNK_OFFSET	(CHUNKFS_CHUNK_BLK * CHUNKFS_BLK_SIZE)
#define CHUNKFS_CHUNK_SIZE	(10 * 1024 * 1024) /* XXX should be dynamic */

static inline int check_chunk(struct chunkfs_chunk *chunk)
{
	return check_metadata(chunk, sizeof(*chunk), CHUNKFS_CHUNK_MAGIC);
}

/*
 * Chunk flags
 */

#define	CHUNKFS_ROOT		0x00000001ULL

#ifdef __KERNEL__

/*
 * XXX Audit client file systems for start-from-zero block address bugs
 *
 * XXX Root inode location? Copy to all chunks? O(n chunk) space usage...
 */

struct chunkfs_chunk_info {
	struct chunkfs_dev_info *ci_dev; /* Parent device */
	struct list_head ci_clist;	/* Member of list of chunks */
	struct buffer_head *ci_bh;
	struct super_block *ci_sb;	/* Superblock of client fs in memory */
	struct vfsmount *ci_mnt;
	__u64 ci_flags;
	__u64 ci_chunk_id;
	char ci_client_fs[CHUNKFS_CLIENT_NAME_LEN];
	/* The rest of the on-disk data is not normally used. */
};

#define CHUNKFS_IS_ROOT(ci)	(ci->ci_flags & CHUNKFS_ROOT)

static inline struct chunkfs_chunk * CHUNKFS_CHUNK(struct chunkfs_chunk_info *ci)
{
	return (struct chunkfs_chunk *) ci->ci_bh->b_data;
}

static inline struct super_block * CHUNKFS_ROOT_SB(struct chunkfs_pool_info *pi)
{
	return pi->pi_root_dev->di_root_chunk->ci_sb;
}

struct chunkfs_chunk_info * chunkfs_find_chunk(struct chunkfs_pool_info *, u64);

#endif /* __KERNEL__ */
