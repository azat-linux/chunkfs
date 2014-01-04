/*
 * Chunkfs continuation routines
 *
 * (C) 2007-2008 Valerie Henson <val@nmt.edu>
 *
 */

#include <linux/xattr.h>
#include "chunkfs.h"
#include "chunkfs_pool.h"
#include "chunkfs_dev.h"
#include "chunkfs_chunk.h"
#include "chunkfs_i.h"

/*
 * Wow... all hack, all the time.  Don't try this at home, kids.
 */

static int
get_set_cont_data(struct dentry *dentry, char *name, u64 value,
		   u64 *ret_value, int type)
{
	/* Yaaaaaaay potential buffer overflow */
	char value_str[50]; /* XXX */
	char full_name[50]; /* XXX */
	/* Our continuation data is stored in the "user" xattr namespace */
	char prefix[] = "user.";
	ssize_t size;
	int err = 0;
	/* printk(KERN_ERR "%s(%s): inode %lu ",  __FUNCTION__,
	   type ? "set" : "get", client_inode->i_ino, name); */
	/* Make our "user.name" xattr name */
	sprintf(full_name, "%s%s", prefix, name);
	if (type == 0) {
		size = generic_getxattr(dentry, full_name, value_str,
				sizeof(value_str));
		if (size >= 0) {
			/* No automatic null termination... */
			value_str[size] = '\0';
			*ret_value = simple_strtoull(value_str, NULL, 10);
		} else {
			err = size;
		}
		/* printk("%s=%llu ", name, *ret_value); */
	} else {
		/* XXX Pad out to max number of characters to avoid ENOSPC */
		sprintf(value_str, "%llu", value);
		err = generic_setxattr(dentry, full_name, value_str,
				       strlen(value_str) + 1, 0);
		/* printk("%s=%s ", name, value_str); */
	}
	/* XXX ENOSPC handling */
	/* printk("err %d\n", err); */
	return err;
}

static int
set_cont_data(struct dentry *dentry, struct chunkfs_cont_data *cd)
{
	int err;

	err = get_set_cont_data(dentry, "next", cd->cd_next, NULL, 1);
	if (err)
		goto out;
	err = get_set_cont_data(dentry, "prev", cd->cd_prev, NULL, 1);
	if (err)
		goto out;
	err = get_set_cont_data(dentry, "start", cd->cd_start, NULL, 1);
	if (err)
		goto out;
	err = get_set_cont_data(dentry, "len", cd->cd_len, NULL, 1);
	if (err)
		goto out;

	mark_inode_dirty(dentry->d_inode);
 out:
	printk(KERN_ERR "%s: inode %lu err %d next %llu prev %llu "
	       "start %llu len %llu\n",
	       __FUNCTION__, dentry->d_inode->i_ino, err,
	       cd->cd_next, cd->cd_prev,
	       cd->cd_start, cd->cd_len);
	return err;
}

/*
 * Get the continuation info out of the underlying client inode and
 * stick it into the continuation info for an element of the inode
 * list for a chunkfs inode.  Currently stored in an xattr, so can use
 * nice pretty fs-independent xattr routines.
 */

static int
get_cont_data(struct dentry *dentry, struct chunkfs_cont_data *cd)
{
	int err;

	err = get_set_cont_data(dentry, "next", 0, &cd->cd_next, 0);
	if (err)
		return err;
	err = get_set_cont_data(dentry, "prev", 0, &cd->cd_prev, 0);
	if (err)
		return err;
	err = get_set_cont_data(dentry, "start", 0, &cd->cd_start, 0);
	if (err)
		return err;
	err = get_set_cont_data(dentry, "len", 0, &cd->cd_len, 0);
	if (err)
		return err;

	printk(KERN_ERR "%s: inode %lu err %d next %llu prev %llu "
	       "start %llu len %llu\n",
	       __FUNCTION__, dentry->d_inode->i_ino, err,
	       cd->cd_next, cd->cd_prev,
	       cd->cd_start, cd->cd_len);

	return 0;
}

/*
 * inode based interface to get cont data
 */

static int
get_cont_data_inode(struct inode *inode, struct chunkfs_cont_data *cd)
{
	struct dentry fake_dentry;
	int err;

	fake_dentry.d_inode = inode;
	fake_dentry.d_sb = inode->i_sb;
	err = get_cont_data(&fake_dentry, cd);
	return err;
}

/*
 * Read an existing continuation into memory.
 *
 * XXX - dget/iget on client?
 */

static int
load_continuation(struct inode *head_inode, struct dentry *client_dentry,
		  u64 chunk_id, struct chunkfs_continuation **ret_cont)
{
	struct chunkfs_pool_info *pi = CHUNKFS_PI(head_inode->i_sb);
	struct chunkfs_continuation *cont;
	struct chunkfs_chunk_info *ci;
	int err;

	printk(KERN_ERR "%s() chunk_id %llu\n", __FUNCTION__, chunk_id);

	cont = kzalloc(sizeof(*cont), GFP_ATOMIC);
	if (cont == NULL)
		return -ENOMEM;

	cont->co_inode = client_dentry->d_inode;
	cont->co_dentry = client_dentry;
	cont->co_chunk_id = chunk_id;
	/* Hm.  Think I could pass in the mnt, too... */
	ci = chunkfs_find_chunk(pi, chunk_id);
	BUG_ON(ci == NULL); /* XXX */
	cont->co_mnt = ci->ci_mnt;
	cont->co_uino = MAKE_UINO(chunk_id, cont->co_inode->i_ino);

	err = get_cont_data(cont->co_dentry, &cont->co_cd);
	if (err)
		goto out;

	*ret_cont = cont;
	return 0;
 out:
	kfree(cont);
	return err;
}

void
chunkfs_put_continuation(struct chunkfs_continuation *cont)
{
	dput(cont->co_dentry);
	/* Should be doing mntput but don't do mntget either */
	kfree(cont);
}

/*
 * Inode list lock must be held.
 *
 * Huuuuuge simplification - only load a continuation into memory
 * while it's being used.  No in-memory linked list.
 *
 */

int
chunkfs_get_next_cont(struct dentry *head_dentry,
		      struct chunkfs_continuation *prev_cont,
		      struct chunkfs_continuation **next_cont)
{
	struct inode *head_inode = head_dentry->d_inode;
	struct chunkfs_cont_data *cd;
	struct dentry *client_dentry;
	struct nameidata nd;
	char path[PATH_MAX];
	u64 from_chunk_id;
	u64 chunk_id;
	u64 from_ino;
	u64 next_uino;
	int err;

	printk(KERN_ERR "%s() prev_cont %p\n", __FUNCTION__, prev_cont);

	/*
	 * Get the dentry for the continuation we want.
	 */

	if (prev_cont == NULL) {
		client_dentry = dget(get_client_dentry(head_dentry));
		chunk_id = UINO_TO_CHUNK_ID(head_inode->i_ino);
	} else {
		cd = &prev_cont->co_cd;
		/* If it's the head inode again, return */
		if (cd->cd_next == head_inode->i_ino) {
			*next_cont = NULL;
			return 0;
		}
		/* If there is no next continuation, return */
		if (cd->cd_next == 0) {
			*next_cont = NULL;
			return 0;
		}
		/* Laboriously construct the path and look it up */
		next_uino = cd->cd_next;
		chunk_id = UINO_TO_CHUNK_ID(next_uino);
		from_chunk_id = prev_cont->co_chunk_id;
		from_ino = UINO_TO_INO(prev_cont->co_uino);
		sprintf(path, "/chunk%llu/%llu/%llu",
			chunk_id, from_chunk_id, from_ino);
		err = kern_path(path, 0, &nd.path);
		if (err)
			return -ENOENT;
		client_dentry = dget(nd.path.dentry);
		path_put(&nd.path);
	}

	/* Now we know the dentry of the continuation we want. */

	err = load_continuation(head_inode, client_dentry, chunk_id,
				next_cont);

	printk(KERN_ERR "%s() returning err %d\n", __FUNCTION__, err);

	return err;
}

int
chunkfs_get_cont_at_offset(struct dentry *dentry, loff_t offset,
			   struct chunkfs_continuation **ret_cont)
{
	struct chunkfs_inode_info *ii = CHUNKFS_I(dentry->d_inode);
	struct chunkfs_continuation *prev_cont = NULL;
	struct chunkfs_continuation *next_cont;
	struct chunkfs_cont_data *cd;
	int err;

	printk(KERN_ERR "%s() reading ino %0lx offset %llu\n",
		__FUNCTION__, dentry->d_inode->i_ino, offset);

	spin_lock(&ii->ii_continuations_lock);
	while (1) {
		err = chunkfs_get_next_cont(dentry, prev_cont, &next_cont);
		if (err || (next_cont == NULL))
			break;
		cd = &next_cont->co_cd;
		printk(KERN_ERR "offset %llu start %llu len %llu err %d\n",
		       offset, cd->cd_start, cd->cd_len, err);
		if ((offset >= cd->cd_start) &&
		    (offset < (cd->cd_start + cd->cd_len))) {
			printk(KERN_ERR "found it!\n");
			*ret_cont = next_cont;
			break;
		}
		printk(KERN_ERR "not this one\n");
		prev_cont = next_cont;
	}
	spin_unlock(&ii->ii_continuations_lock);
	/* If we didn't find a cont at all, return -ENOENT */
	if (next_cont == NULL)
		err = -ENOENT;
	*ret_cont = next_cont;
	return err;
}

/*
 * Traverse the list of continuations using iget() only.
 */

int
chunkfs_get_next_inode(struct inode *head_inode,
		       struct inode *prev_inode, struct inode **ret_inode)
{
	struct chunkfs_pool_info *pi = CHUNKFS_PI(head_inode->i_sb);
	struct chunkfs_chunk_info *ci;
	struct chunkfs_cont_data cd;
	struct inode *next_inode;
	u64 next_uino;
	ino_t next_ino;
	u64 chunk_id;
	int err;

	/* Starting the list... */
	if (prev_inode == NULL) {
		prev_inode = get_client_inode(head_inode);
		next_inode = iget_locked(prev_inode->i_sb, prev_inode->i_ino);
		BUG_ON(!next_inode);
		goto found_inode;
	} else
		iput(prev_inode);
	/* Find the superblock and inode for the next one */
	err = get_cont_data_inode(prev_inode, &cd);
	if (err)
		return err;
	next_uino = cd.cd_next;
	if (next_uino == 0) {
		*ret_inode = NULL;
		return 0;
	}
	next_ino = UINO_TO_INO(next_uino);
	chunk_id = UINO_TO_CHUNK_ID(next_uino);
	printk(KERN_ERR "next_uino %llu next_ino %lu, next chunk_id %llu\n",
	       next_uino, next_ino, chunk_id);
	ci = chunkfs_find_chunk(pi, chunk_id);
	BUG_ON(ci == NULL); /* XXX */
	next_inode = iget_locked(ci->ci_sb, next_ino);
	BUG_ON(!next_inode);
 found_inode:
	unlock_inode(next_inode);

	if (is_bad_inode(next_inode))
		return -EIO;
	*ret_inode = next_inode;
	return 0;
}

/*
 * Create a new continuation in this chunk.  Never called on the head.
 * Length is set arbitrarily so be sure to write continuously.
 *
 * We have to bootstrap ourselves up, starting with a dentry.  We are,
 * in fact, creating a file from the kernel.  Bleah.
 */

int
chunkfs_create_continuation(struct file *file, loff_t *ppos,
			    struct file **client_file,
			    struct chunkfs_continuation **ret_cont)
{
	struct chunkfs_continuation *prev_cont = NULL;
	struct chunkfs_continuation *next_cont;
	struct chunkfs_continuation *new_cont;
	struct file *new_file;
	u64 from_chunk_id;
	u64 to_chunk_id;
	u64 from_ino;
	char path[PATH_MAX];
	struct dentry *dentry;
	struct chunkfs_cont_data cd;
	struct filename filename = { .name = path };
	int err;

	printk(KERN_ERR "%s()\n", __FUNCTION__);

	/* Get the last continuation */
	while (1) {
		err = chunkfs_get_next_cont(file->f_dentry, prev_cont,
					    &next_cont);
		if (err)
			return err;
		if (next_cont == NULL)
			break;
		prev_cont = next_cont;
	}

	/* Figure out what chunk and inode we are continuing from. */
	from_chunk_id = prev_cont->co_chunk_id;
	from_ino = UINO_TO_INO(prev_cont->co_uino);
	/* Temporary hack, do the next chunk for creation. */
	to_chunk_id = from_chunk_id + 1;
	printk(KERN_ERR "%s() to chunk %llu\n", __FUNCTION__, to_chunk_id);

	/* Now we need the filename for the continuation inode. */
	sprintf(path, "/chunk%llu/%llu/%llu", to_chunk_id, from_chunk_id,
		from_ino);

	/* Create the file */
	new_file = file_open_name(&filename, O_CREAT | O_RDWR, MAY_WRITE | MAY_READ | MAY_APPEND);
	if (IS_ERR(new_file)) {
		err = PTR_ERR(new_file);
		printk(KERN_ERR "open_namei for %s: err %d\n", path, err);
		printk(KERN_ERR "dentry_open: err %d\n", err);
		goto out;
	}
	*client_file = new_file;

	dentry = dget(new_file->f_dentry);

	/* Fill in next/prev/etc. data */
	cd.cd_next = 0;
	cd.cd_prev = prev_cont->co_uino;
	cd.cd_start = prev_cont->co_cd.cd_start + prev_cont->co_cd.cd_len;
	cd.cd_len = 10 * 4096;
	set_cont_data(dentry, &cd);
	/* Now update prev */
	prev_cont->co_cd.cd_next = MAKE_UINO(to_chunk_id,
					     dentry->d_inode->i_ino);
	set_cont_data(prev_cont->co_dentry, &prev_cont->co_cd);
	/* Now! It's all in the inode and we can load it like normal. */
	err = load_continuation(file->f_dentry->d_inode, dentry,
				to_chunk_id, &new_cont);

	chunkfs_copy_down_file(file, ppos, new_file, new_cont->co_cd.cd_start);

	*ret_cont = new_cont;

	printk(KERN_ERR "%s(): start %llu returning %d\n",
	       __FUNCTION__, cd.cd_start, err);
	return 0;
 out:
	chunkfs_put_continuation(prev_cont);
	printk(KERN_ERR "%s(): start %llu returning %d\n",
	       __FUNCTION__, cd.cd_start, err);
	return err;
}

int
chunkfs_init_cont_data(struct dentry *client_dentry)
{
	struct chunkfs_cont_data cd;
	int err;

	cd.cd_prev = 0;
	cd.cd_next = 0;
	cd.cd_start = 0;
	cd.cd_len = 10 * 4096;
	err = set_cont_data(client_dentry, &cd);
	return err;
}
