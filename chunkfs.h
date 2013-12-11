/*
 * Chunkfs header file
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 *
 */

#ifndef _LINUX_CHUNKFS_FS_H
#define _LINUX_CHUNKFS_FS_H

/* XXX Do above _H stuff for other header files */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/limits.h>

/*
 * NOTE: Most on disk structures need:
 *
 * Magic number (32 bit is really big but easy)
 * Checksum (32 bit for some kind of sanity)
 *
 * These go first (magic, then checksum) in all on-disk structures so
 * that even if have the type of the structure wrong, we're checking
 * the correct spot.
 *
 * XXX File system generation number should be included - perhaps high
 * 16 of magic?
 */

/* XXX Should have magic, checksum, version, and generation in one struct */

/*
 * Some useful typedefs to possibly prevent endian mixups.  Use c_*
 * for on disk, ci_* for in memory.
 */

typedef __le64 c_blk_t;
typedef __u64 ci_blk_t;
typedef __le64 c_byte_t;
typedef __u64 ci_byte_t;
typedef __le64 c_inode_num_t;
typedef __u64 ci_inode_num_t;

/*
 * XXX Block size shouldn't have much meaning, and it will probably
 * vary by chunk.  Figure out what Linux VFS thinks this means.
 *
 * I think this means that when you use sb_read(), this is the block
 * size used.
 */

#define CHUNKFS_BLK_SIZE	4096
#define CHUNKFS_BLK_BITS	12

/*
 * Rev me! Lots! Whenever on-disk structures change!  Mainly for
 * development.
 *
 * Note that 0 is never acceptable.
 */

#define	CHUNKFS_VERSION	1

/*
 * XXX On-disk structures probably aren't correctly padded at any
 * given moment in time.
 */

/*
 * Locating a device has two parts.  First, we try a cached path name
 * which is a hint only, since paths may change.  Then we check for
 * the correct UUID; if it is wrong, we go search each device.
 */

#define CHUNKFS_DEV_PATH_LEN	1024

struct chunkfs_dev_desc {
	/*
	 * The path of the device when last opened. It may have
	 * changed, therefore it is only a hint.
	 */
	char d_hint[CHUNKFS_DEV_PATH_LEN];
	/* Is this the device we're looking for? */
	__le64 d_uuid;
};

/*
 * Dummy struct to force us to check the "official" position of the
 * checksum and magic number (at the beginning of the struct).
 *
 * XXX Actually use this struct in other structs.  Never access
 * directly.
 */

struct chunkfs_chkmagic {
	__le32 x_magic;
	__le32 x_chksum;
};

/* XXX use e2fsprogs/dev uuid and crc32 lib functions */
/* XXX using __cpu_to_* so userland can share */
/* #ifdef KERNEL? How does Jeff do it? */

static inline void write_chksum(void *buf, unsigned int size)
{
	struct chunkfs_chkmagic *x = (struct chunkfs_chkmagic *) buf;
/*	x->x_chksum = __cpu_to_le32(crc32(buf, size)); */
	x->x_chksum = __cpu_to_le32(0x32323232);
}

static inline int check_chksum(void *buf, unsigned int size)
{
	struct chunkfs_chkmagic *x = (struct chunkfs_chkmagic *) buf;
/*	return !(x->x_chksum == __cpu_to_le32(crc32(buf, size))); */
	return (__le32_to_cpu(x->x_chksum) != 0x32323232);
}

static inline int check_magic(void *buf, __u32 expected_magic) {
	struct chunkfs_chkmagic *x = (struct chunkfs_chkmagic *) buf;
	return (__le32_to_cpu(x->x_magic) != expected_magic);
}
/*
 * Generic function to check a piece of metadata just read off disk.
 * Checksum and magic number are -always- in the same location in all
 * metadata.
 */

static inline int check_metadata(void *buf, unsigned int size, __u32 expected_magic)
{
	if (check_magic(buf, expected_magic))
		return 1;
	if (check_chksum(buf, size))
		return 2;
	return 0;
}

#ifdef __KERNEL__

/* dir.c */
extern struct file_operations chunkfs_dir_fops;

/* inode.c */
extern struct file_operations chunkfs_file_fops;
extern struct inode_operations chunkfs_file_iops;
int chunkfs_new_inode(struct super_block *, struct inode **);
void chunkfs_start_inode(struct inode *inode, struct inode *client_inode,
			 u64 chunk_id);
struct inode *chunkfs_iget(struct super_block *sb, unsigned long ino);
int chunkfs_write_inode(struct inode *inode, struct writeback_control *wbc);
void chunkfs_copy_up_inode(struct inode *, struct inode *);

/* symlink.c */

extern struct inode_operations chunkfs_symlink_iops;

/* namei.c */

extern struct inode_operations chunkfs_dir_iops;
extern struct inode_operations chunkfs_special_iops;

struct chunkfs_dlist_node *chunkfs_alloc_dlist_node(struct dentry *);
void chunkfs_add_dentry(struct dentry *, struct dentry *, struct vfsmount *);
int chunkfs_init_dentry(struct dentry *);
void chunkfs_free_dentry(struct dentry *);
void chunkfs_init_nd(struct inode *dir, struct dentry *dentry,
		    struct dentry *client_dentry, u64 chunk_id);
void chunkfs_copy_up_nd(struct nameidata *nd, struct nameidata *client_nd);
void chunkfs_copy_down_nd(struct nameidata *nd, struct nameidata *client_nd);

/* file.c */

int chunkfs_setattr(struct dentry *dentry, struct iattr *attr);
int chunkfs_permission(struct inode *, int, struct nameidata *);
int chunkfs_open(struct inode *, struct file *);

struct chunkfs_continuation;

int chunkfs_open_cont_file(struct file *file, loff_t *ppos,
			   struct file **client_file,
			   struct chunkfs_continuation **ret_cont);
void chunkfs_close_cont_file(struct file *file, struct file *client_file,
			     struct chunkfs_continuation *cont);
void chunkfs_copy_down_file(struct file *file, loff_t *ppos,
			    struct file *client_file, u64 client_start);

/* cont.c */

int chunkfs_get_next_inode(struct inode *head_inode,
			   struct inode *prev_inode, struct inode **ret_inode);
int chunkfs_get_cont_at_offset(struct dentry *dentry, loff_t offset,
			       struct chunkfs_continuation **ret_cont);
int chunkfs_get_next_cont(struct dentry *head_dentry,
			  struct chunkfs_continuation *prev_cont,
			  struct chunkfs_continuation **next_cont);
int chunkfs_create_continuation(struct file *file, loff_t *ppos,
				struct file **client_file,
				struct chunkfs_continuation **ret_cont);
void chunkfs_put_continuation(struct chunkfs_continuation *cont);
int chunkfs_init_cont_data(struct dentry *client_dentry);

#endif	/* __KERNEL__ */

#endif	/* _LINUX_CHUNKFS_FS_H */
