/*
 * Chunkfs file routines
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 */

#include <linux/module.h>
#include <linux/security.h>
#include <linux/quotaops.h>
#include <linux/file.h>

#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

/*
 * The point of all these wrapper functions is the following:
 *
 * We need to get set the right file ops in the file struct for the
 * area of the file being altered.
 *
 * For ops which affect the entire file (like fsync), we need to fan
 * out to all the parts of the file.
 *
 * It would be cool if we could set the file operations depending on
 * where in the file the I/O is happening.  But I don't think we have
 * that ability in the VFS right now.
 */

/*
 * Set the client file position to be relative to the start of the
 * client file and copy down the main file struct's data in to the
 * client file struct.
 */

void
chunkfs_copy_down_file(struct file *file, loff_t *ppos,
		       struct file *client_file, u64 client_start)
{
	client_file->f_pos = *ppos - client_start;
	*ppos = client_file->f_pos;

	printk(KERN_ERR "%s(): client f_pos set to %llu "
	       "(parent %llu, client_start %llu)\n",
	       __FUNCTION__, client_file->f_pos, file->f_pos,
	       client_start);
}

/*
 * Aaaand reverse the pos conversion.
 */

static void
copy_up_file(struct file *file, struct file *client_file, u64 client_start)
{
	file->f_pos = client_file->f_pos + client_start;

	printk(KERN_ERR "%s(): file f_pos set to %llu (client f_pos %llu "
	       "client_start %llu)\n", __FUNCTION__, file->f_pos,
	       client_file->f_pos, client_start);
}

/*
 * Open the client inode at offset and return the file struct.
 */

int
chunkfs_open_cont_file(struct file *file, loff_t *ppos,
		       struct file **client_file,
		       struct chunkfs_continuation **ret_cont)
{
	struct chunkfs_continuation *cont;
	struct chunkfs_cont_data *cd;
	struct file *new_file;
	/* TODO: embed struct path into chunkfs_continuation */
	struct path co_path = { .mnt = cont->co_mnt, .dentry = cont->co_dentry };
	int err;

	printk(KERN_ERR "%s() pos %llu\n", __FUNCTION__, *ppos);

	err = chunkfs_get_cont_at_offset(file->f_dentry, *ppos, &cont);
	if (err)
		return err;

	new_file = dentry_open(&co_path, file->f_flags, file->f_cred);
	if (IS_ERR(new_file)) {
		err = PTR_ERR(new_file);
		printk(KERN_ERR "dentry_open: err %d\n", err);
		goto out;
	}
	cd = &cont->co_cd;
	chunkfs_copy_down_file(file, ppos, new_file, cd->cd_start);

	*ret_cont = cont;
	*client_file = new_file;
 out:
	printk(KERN_ERR "%s(): returning %d\n", __FUNCTION__, err);
	return err;
}

void
chunkfs_close_cont_file(struct file *file, struct file *client_file,
			   struct chunkfs_continuation *cont)
{
	struct chunkfs_cont_data *cd = &cont->co_cd;
	/* XXX... sys_close does a lot more than this. */
	printk(KERN_ERR "%s()\n", __FUNCTION__);
	copy_up_file(file, client_file, cd->cd_start);
	chunkfs_copy_up_inode(file->f_dentry->d_inode,
			      client_file->f_dentry->d_inode);
	chunkfs_put_continuation(cont);
}

/*
 * lseek only affects the top-level file struct's fpos.
 */

static loff_t
chunkfs_llseek_file(struct file *file, loff_t offset, int origin)
{
	printk(KERN_ERR "%s()\n", __FUNCTION__);

	/* XXX right generic llseek? */
	return default_llseek(file, offset, origin);
}

/*
 * Find the right inode for the offset and read from it.  Opens and
 * closes the client file struct every time because I'm lazy.
 */

static ssize_t
chunkfs_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	struct file *client_file;
	struct chunkfs_continuation *cont;
	int err;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	err = chunkfs_open_cont_file(file, ppos, &client_file, &cont);
	/* Read off the end of the file */
	/* XXX distinguish between this and EIO */
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;

	/* XXX assume not longer than len */
	if (client_file->f_op->read)
		err = client_file->f_op->read(client_file, buf, len, ppos);
	else
		err = do_sync_read(client_file, buf, len, ppos);

	/* If we read off the end, no problemo */
	if (err == -ENODATA)
		err = 0;

	chunkfs_close_cont_file(file, client_file, cont);
	return err;
}

static ssize_t
chunkfs_write(struct file *file, const char __user *buf, size_t len,
	      loff_t *ppos)
{
	struct chunkfs_continuation *cont;
	struct file *client_file;
	ssize_t size;
	int err;

	printk(KERN_ERR "%s() pos %llu len %u\n",
	       __FUNCTION__, *ppos, len);

	err = chunkfs_open_cont_file(file, ppos, &client_file, &cont);
	if (err == -ENOENT) {
		err = chunkfs_create_continuation(file, ppos, &client_file,
						  &cont);
	}
	if (err)
		return err;

	/* XXX assume not longer than len */
	if (client_file->f_op->write)
		size = client_file->f_op->write(client_file, buf, len, ppos);
	else
		size = do_sync_write(client_file, buf, len, ppos);

	chunkfs_close_cont_file(file, client_file, cont);

	printk(KERN_ERR "%s() pos %llu len %u, returning size %u\n",
	       __FUNCTION__, *ppos, len, size);

	return size;
}

/*
 * Open only affects the top-level chunkfs file struct.  Do an open of
 * the underlying head client inode just to see that we can, then
 * close it again.
 */

int
chunkfs_open(struct inode * inode, struct file * file)
{
	struct file *client_file;
	struct chunkfs_continuation *cont;
	loff_t dummy_pos = 0;
	int err;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	err = chunkfs_open_cont_file(file, &dummy_pos, &client_file, &cont);
	if (err)
		goto out;
	chunkfs_close_cont_file(file, client_file, cont);
	return 0;
 out:
	printk(KERN_ERR "%s() returning %d\n", __FUNCTION__, err);
	return err;
}

/*
 * Apparently, file may be null at this point.  Uh.  Whatever.
 */

static int
chunkfs_fsync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(file->f_dentry->d_inode);
	struct chunkfs_continuation *prev_cont = NULL;
	struct chunkfs_continuation *next_cont;
	struct dentry *client_dentry;
	struct inode *client_inode;
	struct file client_file;
	int err = -EIO;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	/* XXX syncs all inodes instead of just ones in mem */
	spin_lock(&ii->ii_continuations_lock);
	while (1) {
		err = chunkfs_get_next_cont(file->f_dentry, prev_cont, &next_cont);
		if (err || (next_cont == NULL))
			break;
		client_dentry = next_cont->co_dentry;
		client_inode = client_dentry->d_inode;

		client_file.f_dentry = client_dentry;
		client_file.f_inode = client_inode;
		/* XXX error propagation */
		err = client_inode->i_fop->fsync(&client_file, start, end, datasync);
		prev_cont = next_cont;
	}
	spin_unlock(&ii->ii_continuations_lock);
	printk(KERN_ERR "%s() err %d\n", __FUNCTION__, err);
	return err;
}

int chunkfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *client_inode = get_client_inode(dentry->d_inode);
	struct dentry *client_dentry = get_client_dentry(dentry);
	unsigned int ia_valid = attr->ia_valid;
	int error;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	if (client_inode->i_op->setattr) {
		error = client_inode->i_op->setattr(client_dentry, attr);
	} else {
		/* Arrrrrgh gross argh */
		error = inode_change_ok(client_inode, attr);
		if (!error)
			error = security_inode_setattr(client_dentry, attr);
		if (!error)
			error = inode_setattr(client_inode, attr);
	}
	if (!error)
		chunkfs_copy_up_inode(dentry->d_inode, client_inode);
	return error;
}

/*
 * XXX probably need to change the nd, that was here before
 */

int chunkfs_permission(struct inode *inode, int submask)
{
	struct inode *client_inode = get_client_inode(inode);
	int err;

	if (client_inode->i_op->permission)
		err = client_inode->i_op->permission(client_inode, submask);
	else
		err = generic_permission(client_inode, submask);
	return err;
}

struct file_operations chunkfs_file_fops = {
	.llseek		= chunkfs_llseek_file,
	.read		= chunkfs_read,
	.write		= chunkfs_write,
	.open		= chunkfs_open,
	.fsync		= chunkfs_fsync_file,
};

struct inode_operations chunkfs_file_iops = {
	.setattr	= chunkfs_setattr,
	.permission	= chunkfs_permission,
};
