/*
 * Chunkfs inode routines
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

#include <linux/module.h>
#include <linux/fs_stack.h>

#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

int
chunkfs_get_nlinks(struct inode *inode)
{
	/* XXX go through all client inodes */
	return inode->i_nlink;
}

static void
__copy_inode(struct inode *dst, struct inode *src)
{
	/* Copy data from one inode to another */
	fsstack_copy_attr_all(dst, src, chunkfs_get_nlinks);
}

void
chunkfs_copy_up_inode(struct inode *inode, struct inode *client_inode)
{
	struct inode *prev_inode = NULL;
	struct inode *next_inode;
	loff_t total_size = 0;

	__copy_inode(inode, client_inode);

	while (1) {
		chunkfs_get_next_inode(inode, prev_inode, &next_inode);
		if (next_inode == NULL)
			break;
		/* XXX doesn't do holey files right */
		printk(KERN_ERR "adding %llu\n", next_inode->i_size);
		total_size += next_inode->i_size;
		prev_inode = next_inode;
	}
	inode->i_size = total_size;
	printk(KERN_ERR "%s() ino %lu size %llu\n", __FUNCTION__,
	       inode->i_ino, inode->i_size);

	mark_inode_dirty(inode);
}

static void
copy_down_inode(struct inode *inode, struct inode *client_inode)
{
	__copy_inode(client_inode, inode);
}

static void
set_inode_ops(struct inode *inode, struct inode *client_inode)
{
	/* Pick inode ops */
	if (S_ISLNK(client_inode->i_mode))
		inode->i_op = &chunkfs_symlink_iops;
	else if (S_ISDIR(client_inode->i_mode))
		inode->i_op = &chunkfs_dir_iops;
	else if (S_ISREG(client_inode->i_mode))
		inode->i_op = &chunkfs_file_iops;
	else
		inode->i_op = &chunkfs_special_iops;

	/* Use different set of file ops for directories */
	if (S_ISDIR(client_inode->i_mode))
		inode->i_fop = &chunkfs_dir_fops;
	else if (S_ISREG(client_inode->i_mode))
		inode->i_fop = &chunkfs_file_fops;

	/* properly initialize special inodes */
	if (S_ISBLK(client_inode->i_mode) || S_ISCHR(client_inode->i_mode) ||
	    S_ISFIFO(client_inode->i_mode) || S_ISSOCK(client_inode->i_mode))
		init_special_inode(inode, client_inode->i_mode,
				   client_inode->i_rdev);
}

/*
 * Allocate a new inode and do any extra bits to it that aren't
 * covered by the alloc_inode() op (currently none).
 */

int
chunkfs_new_inode(struct super_block *sb, struct inode **inodep)
{
	*inodep = new_inode(sb);

	if (is_bad_inode(*inodep))
		/* XXX hate the inode error return conventions */
		return -EIO;
	return 0;
}

/*
 * We've just read in a client inode.  Fill in the chunkfs inode.
 * Wait to fill in the continuation until the file is opened.
 */

void
chunkfs_start_inode(struct inode *inode, struct inode *client_inode,
		    u64 chunk_id)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(inode);

	BUG_ON(!client_inode);

	ii->ii_client_inode = client_inode;
	inode->i_ino = MAKE_UINO(chunk_id, client_inode->i_ino);
	/* XXX i_mapping? */
	/* XXX check inode checksum, etc. */
	set_inode_ops(inode, client_inode);
	chunkfs_copy_up_inode(inode, client_inode);

	printk(KERN_ERR "%s(): inode %p ino %0lx mode %0x client %p\n",
	       __FUNCTION__, inode, inode->i_ino, inode->i_mode,
	       ii->ii_client_inode);
}

/*
 * Come in with the chunkfs inode.  Fill it in and get the client
 * inode too.
 */

struct inode *chunkfs_iget(struct super_block *sb, unsigned long ino)
{
	struct chunkfs_pool_info *pi;
	struct chunkfs_chunk_info *ci;
	struct inode *client_inode;
	struct super_block *client_sb;
	struct inode *inode;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	u64 chunk_id = UINO_TO_CHUNK_ID(inode->i_ino);
	unsigned long client_ino = UINO_TO_INO(inode->i_ino);

	printk (KERN_ERR "%s() reading ino %0lx client ino %0lx chunk_id "
		"%0llx count %d\n",
		__FUNCTION__, inode->i_ino, client_ino, chunk_id,
		atomic_read(&inode->i_count));

	/* XXX should be chunkfs_get_sb */
	ci = chunkfs_find_chunk(pi, chunk_id);
	BUG_ON(ci == NULL); /* XXX */

	client_sb = ci->ci_sb;
	client_inode = iget(client_sb, client_ino);
	if (is_bad_inode(client_inode)) {
		/* XXX should do something here */
		return;
	}
	chunkfs_start_inode(inode, client_inode, chunk_id);
	return;
}

int chunkfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct inode *client_inode = get_client_inode(inode);
	int err;

	copy_down_inode(inode, client_inode);

	/* XXX will client inodes be written when evicted? think so */
	err = client_inode->i_sb->s_op->write_inode(client_inode, wbc);

	return err;
}
