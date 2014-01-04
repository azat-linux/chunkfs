/*
 * Chunkfs inode definitions
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

#ifdef __KERNEL__
#include <linux/namei.h>
#endif


#define	CHUNKFS_INODE_MAGIC	0x10de10de

/*
 * The on-disk version of the chunkfs continuation data is stored as
 * strings inname/value pairs.  They are:
 *
 * "next" "<inode num>" - next inode in the file
 * "prev" - ditto
 * "start" "<byte offset>" - byte offset of file data in this inode
 * "len" "<number bytes>" - length of file data stored in this inode
 */

/*
 * Inode/chunk number and back again
 */

#define UINO_TO_CHUNK_ID(ino)	((ino & 0xF0000000ULL) >> 28)
#define UINO_TO_INO(ino)	(ino & 0x0FFFFFFFULL)
#define MAKE_UINO(chunk_id, ino)	((chunk_id << 28) | ino)

#ifdef __KERNEL__

struct chunkfs_cont_data {
	ci_inode_num_t cd_next;
	ci_inode_num_t cd_prev;
        ci_byte_t cd_start;
	ci_byte_t cd_len;
};

/*
 * This is the information that must be maintained in memory in
 * addition to the client fs's in-memory inode and the VFS's inode.
 */

struct chunkfs_inode_info {
	/* VFS inode */
	struct inode ii_vnode;
	/* Head client inode - keeps our inode state */
	struct inode *ii_client_inode;
	/* Protects on-disk continuation list */
	spinlock_t ii_continuations_lock;
};

/*
 * Info for each continuation in the file.  Loaded as needed and not
 * cached because that's hard, mkay.
 */

struct chunkfs_continuation {
	struct inode *co_inode;
	struct dentry *co_dentry;
	struct vfsmount *co_mnt;
	struct chunkfs_cont_data co_cd;
	u64 co_chunk_id;
	/* Can be reconstructed */
	u64 co_uino;
};

/*
 * We need a single client dentry hanging off the parent dentry, as
 * well as a client version of the nameidata.
 */

struct chunkfs_dentry_priv {
	struct dentry *dp_client_dentry;
	struct nameidata *dp_client_nd;
};

static inline struct chunkfs_inode_info *CHUNKFS_I(struct inode * inode)
{
	return container_of(inode, struct chunkfs_inode_info, ii_vnode);
}

static inline struct inode *get_client_inode(struct inode *inode)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(inode);
	return ii->ii_client_inode;
}

static inline struct chunkfs_dentry_priv *CHUNKFS_D(struct dentry *dentry) {
	return (struct chunkfs_dentry_priv *) dentry->d_fsdata;
}

static inline struct dentry *get_client_dentry(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	return dp->dp_client_dentry;
}

static inline struct nameidata *get_client_nd(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	/*
	 * XXX locking.  Can we have more than one operation going
	 * forward using a nameidata at the same time?  My first guess
	 * is no.
	 */
	return dp->dp_client_nd;
}

static inline struct vfsmount *get_client_mnt(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	return dp->dp_client_nd->path.mnt;
}

static inline void unlock_inode(struct inode *inode)
{
	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);
}

#endif /* __KERNEL__ */
