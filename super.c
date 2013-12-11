/*
 * Chunkfs
 *
 * Chunks is a file system designed to be checked and repaired in
 * small, mostly independent chunks.  This allows quick recovery from
 * file system corruption.
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 *
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/vfs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mutex.h>

#include <asm/uaccess.h>

#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

static DEFINE_MUTEX(chunkfs_kernel_mutex);

static struct inode *chunkfs_alloc_inode(struct super_block *sb)
{
	/* XXX Make a kmem_cache */
	struct chunkfs_inode_info *ii;
	struct inode *inode;

	ii = kzalloc(sizeof (*ii), GFP_KERNEL);
	if (!ii)
		return NULL;
	/* XXX should be done in cache constructor */
	spin_lock_init(&ii->ii_continuations_lock);
	/* Don't load head  continuation until file open */
	inode = &ii->ii_vnode;
	inode_init_once(inode);
	inode->i_version = 1;

	return inode;
}

static void chunkfs_destroy_inode(struct inode *inode)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(inode);

	printk(KERN_ERR "%s(): ino %0lx i_count %d\n", __FUNCTION__,
	       inode->i_ino, atomic_read(&inode->i_count));

	kfree(ii);
}

static void chunkfs_clear_inode(struct inode *inode)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(inode);

	printk(KERN_ERR "%s(): ino %0lx i_count %d\n",
	       __FUNCTION__, inode->i_ino, atomic_read(&inode->i_count));
	iput(ii->ii_client_inode);
}

static int
chunkfs_read_client_sb(struct chunkfs_chunk_info *ci)
{
	/* XXX XXX XXX There aren't enough XXX's in the world XXX XXX */
	char *path_prefix = "/chunk";
	char mount_path[strlen(path_prefix + 10)];
	struct nameidata nd;
	int retval;

	/*
	 * Userland has kindly mounted our client fs's in particular
	 * locations.  Look up the path and grab the superblock for
	 * each chunk.
	 *
	 * XXX Yuckity yuckity yuck yuck
	 */
	sprintf(mount_path, "%s%llu", path_prefix, ci->ci_chunk_id);
	retval = kern_path(mount_path, LOOKUP_FOLLOW, &nd.path);
	if (retval) {
		printk(KERN_ERR "path_lookup for %s failed: %d\n",
		       mount_path, retval);
		return retval;
	}
	/* XXX locking XXX prevent unmount XXX ref count XXX XXX */
	ci->ci_mnt = mntget(nd.path.mnt);
	ci->ci_sb = nd.path.mnt->mnt_sb;
	path_put(&nd.path);

	return 0;
}

struct chunkfs_chunk_info *
chunkfs_find_chunk(struct chunkfs_pool_info *pi, u64 chunk_id)
{
	struct chunkfs_dev_info *di;
	struct chunkfs_chunk_info *ci;

	list_for_each_entry(di, &pi->pi_dlist_head, di_dlist) {
		list_for_each_entry(ci, &di->di_clist_head, ci_clist) {
			if(ci->ci_chunk_id == chunk_id)
				return ci;
		}
	}
	return NULL;
}

static void chunkfs_free_chunk(struct chunkfs_chunk_info *ci)
{
	brelse(ci->ci_bh);
	mntput(ci->ci_mnt);
	kfree(ci);
}

static void chunkfs_free_dev(struct chunkfs_dev_info *di)
{
	struct chunkfs_chunk_info *ci, *ci_next;

	list_for_each_entry_safe(ci, ci_next, &di->di_clist_head, ci_clist) {
		list_del(&ci->ci_clist);
		chunkfs_free_chunk(ci);
	}
	brelse(di->di_bh);
	kfree(di);
}

static void chunkfs_free_pool(struct chunkfs_pool_info *pi)
{
	struct chunkfs_dev_info *di, *di_next;

	list_for_each_entry_safe(di, di_next, &pi->pi_dlist_head, di_dlist) {
		list_del(&di->di_dlist);
		chunkfs_free_dev(di);
	}
	brelse(pi->pi_bh);
	kfree(pi);
}

static int chunkfs_read_chunk(struct super_block *sb,
			      struct chunkfs_dev_info *dev,
			      struct chunkfs_chunk_info **chunk_info,
			      ci_byte_t chunk_offset,
			      ci_byte_t *next_chunk_offset)
{
	struct chunkfs_chunk_info *ci;
	struct chunkfs_chunk *chunk;
	struct buffer_head *bh;
	int retval = -EIO;
	int err;

	ci = kzalloc(sizeof(*ci), GFP_KERNEL);
	if (!ci)
		return -ENOMEM;

	/* XXX assumes offset is multiple of underlying block size */

	if (!(bh = sb_bread(sb, chunk_offset/CHUNKFS_BLK_SIZE))) {
		printk (KERN_ERR "chunkfs: unable to read chunk summary at %llu",
			chunk_offset);
		goto out_nobh;
	}

	ci->ci_bh = bh;
	chunk = CHUNKFS_CHUNK(ci);

	if ((err = check_chunk(chunk)) != 0) {
		printk (KERN_ERR "chunkfs: Invalid chunk summary, err %d, chksum %0x\n",
			err, le32_to_cpu(chunk->c_chksum));
		goto out;
	}

	/* Fill in on-disk info */
	ci->ci_flags = cpu_to_le64(chunk->c_flags);
	*next_chunk_offset = cpu_to_le64(chunk->c_next_chunk);
	ci->ci_chunk_id = cpu_to_le64(chunk->c_chunk_id);
	memcpy(ci->ci_client_fs, chunk->c_client_fs, CHUNKFS_CLIENT_NAME_LEN);

	/* Init non-disk stuff */
	ci->ci_dev = dev;

	/* Mount the client file system */
	retval = chunkfs_read_client_sb(ci);
	if (retval)
		goto out;

	*chunk_info = ci;
	return 0;
 out:
	brelse(bh);
	ci->ci_bh = NULL;
 out_nobh:
	kfree(ci);
	BUG_ON(retval == 0);
	return retval;
}

static int chunkfs_read_dev(struct super_block *sb,
			    struct chunkfs_pool_info *pool_info,
			    struct chunkfs_dev_info **dev_info)
{
	struct chunkfs_dev_info *di;
	struct chunkfs_dev *dev;
	struct buffer_head * bh;
	struct chunkfs_chunk_info *ci, *ci_next;
	ci_byte_t chunk_offset, next_chunk_offset;
	int retval = -EIO;
	int err;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	/* XXX assumes sb offset is multiple of underlying block size */

	if (!(bh = sb_bread(sb, CHUNKFS_DEV_BLK))) {
		printk (KERN_ERR "chunkfs: unable to read dev summary\n");
		goto out_nobh;
	}

	di->di_bh = bh;
	dev = CHUNKFS_DEV(di);

	if ((err = check_dev(dev)) != 0) {
		printk (KERN_ERR "chunkfs: Invalid dev summary err %d chksum %0x\n",
			err, le32_to_cpu(dev->d_chksum));
		goto out_bh;
	}
	/* Fill in on-disk info */
	di->di_flags = cpu_to_le64(dev->d_flags);
	chunk_offset = cpu_to_le64(dev->d_innards_begin);

	/* Init non-disk stuff */
	INIT_LIST_HEAD(&di->di_clist_head);
	di->di_pool = pool_info;

	/* XXX would like to sanity check dev size here */

	while (chunk_offset != 0) {
		retval = chunkfs_read_chunk(sb, di, &ci, chunk_offset,
					    &next_chunk_offset);
		if (retval)
			goto out_free_chunks;
		list_add_tail(&ci->ci_clist, &di->di_clist_head);
		if (CHUNKFS_IS_ROOT(ci)) {
			BUG_ON(di->di_pool->pi_root_dev);
			di->di_pool->pi_root_dev = di;
			BUG_ON(di->di_root_chunk);
			di->di_root_chunk = ci;
		}
		chunk_offset = next_chunk_offset;
	}

	/* Did we find root? */
	if (!di->di_root_chunk) {
		printk(KERN_ERR "chunkfs: did not find root\n");
		goto out_free_chunks;
	}
	*dev_info = di;
	return 0;
 out_free_chunks:
	list_for_each_entry_safe(ci, ci_next, &di->di_clist_head, ci_clist) {
		list_del(&ci->ci_clist);
		chunkfs_free_chunk(ci);
	}
 out_bh:
	brelse(bh);
	di->di_bh = NULL;
 out_nobh:
	kfree(di);
	return retval;
}

static int chunkfs_read_pool(struct super_block *sb,
			     struct chunkfs_pool_info **pool_info)
{
	struct chunkfs_pool_info *pi;
	struct chunkfs_pool *pool;
	struct buffer_head * bh;
	struct chunkfs_dev_info *di;
	int retval = -EIO;
	int err;

	pi = kzalloc(sizeof(*pi), GFP_KERNEL);
	if (!pi)
		return -ENOMEM;

	/* XXX assumes sb offset is multiple of underlying block size */

	if (!(bh = sb_bread(sb, CHUNKFS_POOL_BLK))) {
		printk (KERN_ERR "chunkfs: unable to read pool summary\n");
		goto out_nobh;
	}

	pi->pi_bh = bh;
	pool = CHUNKFS_POOL(pi);

	if ((err = check_pool(pool)) != 0) {
		printk (KERN_ERR "chunkfs: Invalid pool summary, err %d chksum %0x magic %0x\n",
			err, le32_to_cpu(pool->p_chksum), le32_to_cpu(pool->p_magic));
		goto out;
	}
	/* Fill in on-disk info */
	pi->pi_flags = cpu_to_le64(pool->p_flags);

	/* Init non-disk stuff */
	INIT_LIST_HEAD(&pi->pi_dlist_head);

	/* XXX read multiple devs */
	/* For now, we just read at a particular offset on this dev */
	retval = chunkfs_read_dev(sb, pi, &di);
	if (retval)
		goto out;
	list_add_tail(&di->di_dlist, &pi->pi_dlist_head);

	*pool_info = pi;
	return 0;
 out:
	brelse(bh);
	pi->pi_bh = NULL;
 out_nobh:
	kfree(pi);
	return retval;
}

static void chunkfs_commit_super (struct super_block *sb, int sync)
{
	struct buffer_head *sbh = CHUNKFS_PI(sb)->pi_bh;

	if (!sbh)
		return;
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);
}

static void chunkfs_put_super (struct super_block *sb)
{
	struct chunkfs_pool_info *pi = CHUNKFS_PI(sb);

	if (!(sb->s_flags & MS_RDONLY)) {
		/* XXX should mark super block as clean unmounted */
		chunkfs_commit_super(sb, 1);
	}
	chunkfs_free_pool(pi);
	sb->s_fs_info = NULL;

	return;
}

static int
chunkfs_write_super (struct super_block *sb, int wait)
{
	/* TODO: another mutex in private part of sb. */
	mutex_lock(&chunkfs_kernel_mutex);
	chunkfs_commit_super(sb, 1);
	/* TODO: it is clear now */
	mutex_unlock(&chunkfs_kernel_mutex);

	return 0;
}


static struct super_operations chunkfs_sops = {
	.alloc_inode	= chunkfs_alloc_inode,
	.destroy_inode	= chunkfs_destroy_inode,
	.write_inode	= chunkfs_write_inode,
#if 0 /* XXX Totally unimplemented at present */
	.dirty_inode	= chunkfs_dirty_inode,
	.delete_inode	= chunkfs_delete_inode,
#endif
	.put_super	= chunkfs_put_super,
	/* TODO: sync the whole fs, not just sb. */
	.sync_fs	= chunkfs_write_super,
#if 0
	.sync_fs	= chunkfs_sync_fs,
	.write_super_lockfs = chunkfs_write_super_lockfs,
	.unlockfs	= chunkfs_unlockfs,
	.statfs		= chunkfs_statfs,
	.remount_fs	= chunkfs_remount,
#endif
	.evict_inode	= chunkfs_clear_inode,
#if 0
	.show_options	= chunkfs_show_options,
#endif
};

/*
 * The file system in the root chunk has already been mounted, so the
 * chunk root inode is already loaded and stored in the superblock.
 * However, we really want to have the root directory in terms of the
 * chunkfs namespace, which is presently named "/root" and inode 12.
 */

static int chunkfs_read_root(struct super_block *sb)
{
	struct chunkfs_chunk_info *ci = CHUNKFS_PI(sb)->pi_root_dev->di_root_chunk;
	ino_t ino = MAKE_UINO(ci->ci_chunk_id, 12); /* XXX */
	struct inode *inode;
	struct nameidata nd;
	struct dentry *dentry;
	int retval;

	inode = chunkfs_iget(sb, ino);
	BUG_ON(!inode);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		retval = -ENOMEM;
		goto out_iput;
	}
	retval = chunkfs_init_dentry(sb->s_root);
	if (retval)
		goto out_dput;
	retval = kern_path("/chunk1/root/", LOOKUP_FOLLOW, &nd.path);
	if (retval)
		goto out_dentry;
	dentry = dget(nd.path.dentry);
	chunkfs_init_nd(inode, sb->s_root, dentry, ci->ci_chunk_id);
	chunkfs_add_dentry(sb->s_root, dentry, nd.path.mnt);
	path_put(&nd.path);
	return 0;
 out_dentry:
	chunkfs_free_dentry(sb->s_root);
 out_dput:
	dput(sb->s_root);
 out_iput:
	iput(inode);
	printk(KERN_ERR "%s() path lookup failed\n", __FUNCTION__);
	return retval;
}

/*
 * chunkfs_setup_super does all things that are shared between mount
 * and remount.  At moment, I'm not sure what they are.
 */

static int chunkfs_setup_super(struct super_block *sb,
			       struct chunkfs_pool_info *pi,
			       int read_only)
{
	return 0;
}

/*
 * Get the superblock off the disk and check to see if it is sane.
 *
 * Note that VFS code has a generic routine to find alternate superblocks.
 *
 * XXX todo, put dev summary copies in chunk summaries.
 */

static int chunkfs_fill_super (struct super_block *sb, void *data, int silent)
{
	struct chunkfs_pool_info *pi;
	int retval = -EINVAL;

	mutex_unlock(&chunkfs_kernel_mutex);

	printk(KERN_ERR "%s\n", __FUNCTION__);

	/* We must set blocksize before we can read blocks. */

	if (sb_set_blocksize(sb, CHUNKFS_BLK_SIZE) == 0)
		goto out;

	retval = chunkfs_read_pool(sb, &pi);
	if (retval)
		goto out;
	sb->s_fs_info = pi;

	sb->s_maxbytes = ~0ULL;
	sb->s_op = &chunkfs_sops;

	retval = chunkfs_read_root(sb);
	if (retval)
		goto out;
	/* If fail after this, dput sb->s_root */

	chunkfs_setup_super (sb, pi, sb->s_flags & MS_RDONLY);

	printk(KERN_ERR "chunkfs: mounted file system\n");
	mutex_lock(&chunkfs_kernel_mutex);
	return 0;
 out:
	mutex_lock(&chunkfs_kernel_mutex);
	BUG_ON(retval == 0);
	printk(KERN_ERR "%s() failed! err %d\n", __FUNCTION__, retval);
	return retval;
}

static struct dentry *chunkfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_single(fs_type, flags, data, chunkfs_fill_super);
}

static struct file_system_type chunkfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "chunkfs",
	.mount		= chunkfs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_chunkfs_fs(void)
{
	int err = register_filesystem(&chunkfs_fs_type);
	if (!err)
		printk(KERN_INFO "chunkfs (C) 2007 Valerie Henson "
		       "<val@nmt.edu>\n");
	return err;
}

static void __exit exit_chunkfs_fs(void)
{
	unregister_filesystem(&chunkfs_fs_type);
}

MODULE_AUTHOR("Val Henson");
MODULE_DESCRIPTION("Chunkfs");
MODULE_LICENSE("GPL");
module_init(init_chunkfs_fs)
module_exit(exit_chunkfs_fs)
