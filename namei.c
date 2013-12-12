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

#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

#include <linux/mount.h>
#include <linux/slab.h>

void
chunkfs_release_nd(struct dentry *dentry)
{
	struct nameidata *nd = get_client_nd(dentry);
	dput(nd->path.dentry);
	mntput(nd->path.mnt);
}

/*
 * Call this to initialize our client nameidata.
 */

void
chunkfs_init_nd(struct inode *dir, struct dentry *dentry,
	       struct dentry *client_dentry, u64 chunk_id)
{
	struct nameidata *nd = get_client_nd(dentry);
	struct chunkfs_chunk_info *chunk;

	chunk = chunkfs_find_chunk(CHUNKFS_PI(dir->i_sb), chunk_id);
	BUG_ON(!chunk); /* XXX */
	/* Probably don't need dget/mntget */
	nd->path.dentry = dget(client_dentry);
	nd->path.mnt = mntget(chunk->ci_mnt);
	printk(KERN_ERR "%s(): dentry %p name %s client_dentry %p mnt %s\n",
	       __FUNCTION__, dentry, dentry->d_iname, client_dentry,
	       nd->path.mnt->mnt_sb->s_type->name);
}

/*
 * The client file system may read the following parts of the nameidata:
 *
 * In open, it read the intent's mode or flags.
 *
 * The client file system may alter the nameidata in the following cases:
 *
 * When following symbolic links (up to N levels of links saved in
 * nd), it may set the saved_names (using the depth) with nd_set_link.
 */

static void
__chunkfs_copy_nd(struct nameidata *dst, struct nameidata *src)
{
	dst->flags = src->flags;
	dst->seq   = src->seq;
	dst->depth = src->depth;
	dst->saved_names[dst->depth] = src->saved_names[dst->depth];
}

void
chunkfs_copy_up_nd(struct nameidata *nd, struct nameidata *client_nd)
{
	__chunkfs_copy_nd(nd, client_nd);
}

void
chunkfs_copy_down_nd(struct nameidata *nd, struct nameidata *client_nd)
{
	__chunkfs_copy_nd(client_nd, nd);
}

static void
chunkfs_remove_dentry(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	dput(dp->dp_client_dentry);
}

void
chunkfs_free_dentry(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	kfree(dp->dp_client_nd);
	kfree(dp);
	dentry->d_fsdata = NULL;
}

/*
 * Called when a dentry is evicted from cache.
 */

void
chunkfs_release_dentry(struct dentry *dentry)
{
	printk(KERN_ERR "%s(): name %s\n", __FUNCTION__, dentry->d_name.name);
	/*
	 * Root dentry can be legitimately released on umount, but is
	 * also a common manifestation of refcounting problems.  Catch
	 * for debugging.
	 */
	WARN_ON(strcmp(dentry->d_name.name, "/") == 0);
	chunkfs_release_nd(dentry);
	/*
	 * Negative dentries need client dentries too, so they can be
	 * easily converted into responsible positive dentries.  We
	 * should never have a dentry without a client dentry.
	 */
	chunkfs_remove_dentry(dentry);
	chunkfs_free_dentry(dentry);
}

struct dentry_operations chunkfs_dops = {
	.d_release = chunkfs_release_dentry,
};

/*
 * Initialize a new chunkfs dentry.
 */

int
chunkfs_init_dentry(struct dentry *dentry)
{
	struct chunkfs_dentry_priv *dp;
	struct nameidata *nd;

	BUG_ON(dentry->d_fsdata);
	dp = kzalloc(sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	nd = kzalloc(sizeof(*nd), GFP_KERNEL);
	if (!nd)
		goto out;
	dp->dp_client_nd = nd;
	dentry->d_fsdata = dp;
	dentry->d_op = &chunkfs_dops;
	return 0;
 out:
	kfree(dp);
	return -ENOMEM;
}

/*
 * This function takes a chunkfs dentry and constructs a new dentry
 * for the client fs.
 */

static struct dentry *
chunkfs_clone_dentry(struct dentry *dentry)
{
	struct dentry *client_parent = get_client_dentry(dentry->d_parent);
	struct dentry *client_dentry;

	client_dentry = d_alloc_name(client_parent, dentry->d_name.name);

	if (!client_dentry)
		return ERR_PTR(-ENOMEM);
	return client_dentry;
}

void
chunkfs_add_dentry(struct dentry *dentry, struct dentry *client_dentry,
		   struct vfsmount *mnt)
{
	struct chunkfs_dentry_priv *dp = CHUNKFS_D(dentry);
	dp->dp_client_dentry = client_dentry;
}

static int
chunkfs_create(struct inode *dir, struct dentry *dentry, int mode,
	       struct nameidata *nd)
{
	struct inode *client_dir = get_client_inode(dir);
	struct dentry *client_dentry = get_client_dentry(dentry);
	struct nameidata *client_nd = get_client_nd(dentry);
	u64 chunk_id = UINO_TO_CHUNK_ID(dir->i_ino);
	struct inode *inode;
	int err;

	printk(KERN_ERR "%s(): dir ino %0lx i_count %d\n",
	       __FUNCTION__, dir->i_ino, atomic_read(&dir->i_count));

	err = chunkfs_new_inode(dir->i_sb, &inode);
	if (err)
		goto out;

	chunkfs_copy_down_nd(nd, client_nd);

	err = client_dir->i_op->create(client_dir, client_dentry, mode,
				       client_nd);
	if (err)
		goto out_inode;

	err = chunkfs_init_cont_data(client_dentry);
	if (err)
		goto out_inode;
	chunkfs_start_inode(inode, client_dentry->d_inode, chunk_id);
	chunkfs_copy_up_inode(dir, client_dir);
	chunkfs_copy_up_nd(nd, client_nd);

	/* Now put our new inode into the dentry */
	d_instantiate(dentry, inode);

	printk(KERN_ERR "dentry %p name %s inode %p ino %0lx\n",
	       dentry, dentry->d_iname, dentry->d_inode,
	       dentry->d_inode->i_ino);

	printk(KERN_ERR "client dentry %p name %s inode %p ino %0lx\n",
	       client_dentry, client_dentry->d_iname, client_dentry->d_inode,
	       client_dentry->d_inode->i_ino);
	return 0;
 out_inode:
	iput(inode);
 out:
	return err;
}

static struct dentry *
chunkfs_lookup(struct inode * dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode *client_dir = get_client_inode(dir);
	u64 chunk_id = UINO_TO_CHUNK_ID(dir->i_ino);
	struct dentry *client_dentry;
	struct dentry *new_dentry;
	struct nameidata *client_nd;
	struct inode *inode;
	int err;

	printk(KERN_ERR "%s(): name %s dir ino %0lx i_count %d\n",
	       __FUNCTION__, dentry->d_iname, dir->i_ino,
	       atomic_read(&dir->i_count));

	err = chunkfs_init_dentry(dentry);
	if (err)
		goto out;

	client_dentry = chunkfs_clone_dentry(dentry);
	if (IS_ERR(client_dentry))
		goto out_dentry;

	chunkfs_init_nd(dir, dentry, client_dentry, chunk_id);
	client_nd = get_client_nd(dentry);
	/*
	 * Fill out the client dentry.
	 */
	new_dentry = client_dir->i_op->lookup(client_dir, client_dentry,
					      client_nd);
	/*
	 * Possible return values:
	 *
	 * NULL: Nothing went wrong with lookup, you may or may not
	 * have found a matching inode and attached it.  If the inode
	 * is NULL, we still have to create a negative dentry.
	 *
	 * Address of a dentry: The dentry already existed (and was
	 * root and disconnected - something about knfsd), so the
	 * dentry we passed in needs to be thrown away and we should
	 * use the one returned.
	 *
	 * IS_ERR(): Something went wrong, return the error.
	 */
	if (IS_ERR(new_dentry)) {
		err = PTR_ERR(new_dentry);
		goto out_dput;
	} else if (new_dentry) {
		dput(client_dentry);
		client_dentry = new_dentry;
	}

	/*
	 * If the client found an inode, fill in the chunkfs inode.
	 */
	if (client_dentry->d_inode) {
		err = chunkfs_new_inode(dir->i_sb, &inode);
		if (err)
			goto out_dput;
		err = chunkfs_init_cont_data(client_dentry);
		if (err)
			goto out_dput;
		chunkfs_start_inode(inode, client_dentry->d_inode,
				    chunk_id);
	} else {
		inode = NULL;
	}
	/* Hook up the client and parent dentries. */
	chunkfs_add_dentry(dentry, client_dentry, client_nd->path.mnt);

	printk(KERN_ERR "dentry %p name %s inode %p\n",
	       dentry, dentry->d_iname, dentry->d_inode);
	printk(KERN_ERR "client dentry %p name %s inode %p\n", client_dentry,
	       client_dentry->d_iname, client_dentry->d_inode);

	return d_splice_alias(inode, dentry);
 out_dput:
	dput(client_dentry);
	chunkfs_release_nd(dentry);
 out_dentry:
	chunkfs_remove_dentry(dentry);
 out:
	chunkfs_free_dentry(dentry);

	printk(KERN_ERR "%s(): name %s returning %d\n",
	       __FUNCTION__, dentry->d_iname, err);

	return ERR_PTR(err);
}

static int
chunkfs_link(struct dentry *old_dentry, struct inode *dir,
	     struct dentry *new_dentry)
{
	struct inode *client_dir = get_client_inode(dir);
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *client_old_inode = get_client_inode(old_inode);
	struct dentry *client_old_dentry = get_client_dentry(old_dentry);
	struct dentry *client_new_dentry = get_client_dentry(new_dentry);
	int err = 0;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	err = client_dir->i_op->link(client_old_dentry, client_dir,
				     client_new_dentry);
	if (err)
		goto out;
	/* Copy up inode takes care of link counts */
	chunkfs_copy_up_inode(old_inode, client_old_inode);
	/*
	 * For some reason, this is the one place where the VFS
	 * doesn't increment the inode ref count for us.
	 */
	atomic_inc(&dir->i_count);
	d_instantiate(new_dentry, old_inode);
 out:
	return err;
}

static int
chunkfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *client_dir = get_client_inode(dir);
	struct dentry *client_dentry = get_client_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	struct inode *client_inode = get_client_inode(inode);
	int err = 0;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	err = client_dir->i_op->unlink(client_dir, client_dentry);
	if (err)
		goto out;
	chunkfs_copy_up_inode(dir, client_dir);
	chunkfs_copy_up_inode(inode, client_inode);
 out:
	return err;
}

static int
chunkfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	struct inode *client_dir = get_client_inode(dir);
	struct dentry *client_dentry = get_client_dentry(dentry);
	u64 chunk_id = UINO_TO_CHUNK_ID(dir->i_ino);
	struct inode *inode;
	int err;

	printk(KERN_ERR "%s(): dir ino %0lx i_count %d\n",
	       __FUNCTION__, dir->i_ino, atomic_read(&dir->i_count));

	err = chunkfs_new_inode(dir->i_sb, &inode);
	if (err)
		goto out;

	err = client_dir->i_op->symlink(client_dir, client_dentry, oldname);
	if (err)
		goto out_inode;

	err = chunkfs_init_cont_data(client_dentry);
	if (err)
		goto out_inode;
	chunkfs_start_inode(inode, client_dentry->d_inode, chunk_id);
	chunkfs_copy_up_inode(dir, client_dir);

	/* Now put our new inode into the dentry */
	d_instantiate(dentry, inode);

	printk(KERN_ERR "dentry %p name %s inode %p ino %0lx\n",
	       dentry, dentry->d_iname, dentry->d_inode,
	       dentry->d_inode->i_ino);
	printk(KERN_ERR "client dentry %p name %s inode %p ino %0lx\n",
	       client_dentry, client_dentry->d_iname, client_dentry->d_inode,
	       client_dentry->d_inode->i_ino);
	return 0;
 out_inode:
	iput(inode);
 out:
	return err;
}

static int
chunkfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *client_dir = get_client_inode(dir);
	struct inode *client_inode;
	struct dentry *client_dentry = get_client_dentry(dentry);
	u64 chunk_id = UINO_TO_CHUNK_ID(dir->i_ino);
	struct inode *inode;
	int err;

	printk(KERN_ERR "%s(): name %s dir ino %0lx i_count %d\n",
	       __FUNCTION__, dentry->d_iname, dir->i_ino,
	       atomic_read(&dir->i_count));

	err = chunkfs_new_inode(dir->i_sb, &inode);
	if (err)
		goto out;

	err = client_dir->i_op->mkdir(client_dir, client_dentry, mode);
	if (err)
		goto out_inode;
	client_inode = client_dentry->d_inode;

	err = chunkfs_init_cont_data(client_dentry);
	if (err)
		goto out_inode;
	chunkfs_start_inode(inode, client_inode, chunk_id);
	chunkfs_copy_up_inode(dir, client_dir);
	d_instantiate(dentry, inode);
	return 0;
 out_inode:
	iput(inode);
 out:
	printk(KERN_ERR "%s(): name %s returning %d\n",
	       __FUNCTION__, dentry->d_iname, err);
	return err;
}

static int
chunkfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *client_dir = get_client_inode(dir);
	struct dentry *client_dentry = get_client_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	int err;

	printk(KERN_ERR "%s()\n", __FUNCTION__);
	err = client_dir->i_op->rmdir(client_dir, client_dentry);
	if (err)
		return err;
	chunkfs_copy_up_inode(dir, client_dir);
	chunkfs_copy_up_inode(inode, client_dentry->d_inode);
	return 0;
}

static int
chunkfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	struct inode *client_dir = get_client_inode(dir);
	struct dentry *client_dentry = get_client_dentry(dentry);
	u64 chunk_id = UINO_TO_CHUNK_ID(dir->i_ino);
	struct inode *inode;
	int err;

	printk(KERN_ERR "%s(): name %s dir ino %0lx i_count %d\n",
	       __FUNCTION__, dentry->d_iname, dir->i_ino,
	       atomic_read(&dir->i_count));

	err = chunkfs_new_inode(dir->i_sb, &inode);
	if (err)
		goto out;

	err = client_dir->i_op->mknod(client_dir, client_dentry, mode, dev);
	if (err)
		goto out_inode;

	err = chunkfs_init_cont_data(client_dentry);
	if (err)
		goto out_inode;
	chunkfs_start_inode(inode, client_dentry->d_inode, chunk_id);
	chunkfs_copy_up_inode(dir, client_dir);
	d_instantiate(dentry, inode);

	return 0;
 out_inode:
	iput(inode);
 out:
	printk(KERN_ERR "%s(): name %s returning %d\n",
	       __FUNCTION__, dentry->d_iname, err);
	return err;
}

static int
chunkfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *client_old_dir = get_client_inode(old_dir);
	struct inode *client_new_dir = get_client_inode(new_dir);
	struct dentry *client_old_dentry = get_client_dentry(old_dentry);
	struct dentry *client_new_dentry = get_client_dentry(new_dentry);
	int err = 0;

	return -ENOSYS;
	/* Not reached */
	err = client_old_dir->i_op->rename(client_old_dir,
					      client_old_dentry,
					      client_new_dir,
					      client_new_dentry);
	if (err)
		goto out;
	chunkfs_copy_up_inode(old_dir, client_old_dir);
	chunkfs_copy_up_inode(new_dir, client_new_dir);
 out:
	return err;
}

struct inode_operations chunkfs_dir_iops = {
	.create		= chunkfs_create,
	.lookup		= chunkfs_lookup,
	.link		= chunkfs_link,
	.unlink		= chunkfs_unlink,
	.symlink	= chunkfs_symlink,
	.mkdir		= chunkfs_mkdir,
	.rmdir		= chunkfs_rmdir,
	.mknod		= chunkfs_mknod,
	.rename		= chunkfs_rename,
	.setattr	= chunkfs_setattr,
	.permission	= chunkfs_permission,
};

struct inode_operations chunkfs_special_iops = {
	.setattr	= chunkfs_setattr,
	.permission	= chunkfs_permission,
};
