/*
 * Chunkfs symlinks
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

#include "chunkfs.h"
#include "chunkfs_i.h"

static int
chunkfs_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct inode *client_inode = get_client_inode(dentry->d_inode);
	struct dentry *client_dentry = get_client_dentry(dentry);
	int err;

	chunkfs_debug("enter\n");

	err = client_inode->i_op->readlink(client_dentry, buffer, buflen);

	return err;
}

static void *
chunkfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *client_inode = get_client_inode(dentry->d_inode);
	struct dentry *client_dentry = get_client_dentry(dentry);
	struct nameidata *client_nd = get_client_nd(dentry);
	void *cookie;

	chunkfs_debug("enter\n");

	chunkfs_copy_down_nd(nd, client_nd);

	cookie = client_inode->i_op->follow_link(client_dentry, client_nd);

	chunkfs_copy_up_nd(nd, client_nd);

	return cookie;
}

static void
chunkfs_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
	struct inode *client_inode = get_client_inode(dentry->d_inode);
	struct dentry *client_dentry = get_client_dentry(dentry);
	struct nameidata *client_nd = get_client_nd(dentry);

	chunkfs_debug("enter\n");
	if (client_inode->i_op->put_link) {
		chunkfs_copy_down_nd(nd, client_nd);
		client_inode->i_op->put_link(client_dentry, client_nd, cookie);
		chunkfs_copy_up_nd(nd, client_nd);
	}
}

struct inode_operations chunkfs_symlink_iops = {
	.readlink	= chunkfs_readlink,
	.follow_link	= chunkfs_follow_link,
	.put_link	= chunkfs_put_link,
};
