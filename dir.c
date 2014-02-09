/*
 * Chunkfs directory routines
 *
 * (C) 2007-2008 Valerie Henson <val_henson@linux.intel.com>
 */

#include <linux/module.h>

#include "chunkfs.h"
#include "chunkfs_i.h"

/*
 * Currently we're reusing the client directory ops.  We'll probably
 * have to implement our own directories on top.
 */

static loff_t
chunkfs_dir_llseek(struct file *file, loff_t offset, int origin)
{
	struct file *client_file;
	struct chunkfs_continuation *cont;
	int err;

	/* XXX... should only do top-level file struct? */
	chunkfs_debug("enter\n");

	err = chunkfs_open_cont_file(file, &offset, &client_file, &cont);
	if (err)
		return err;

	if (client_file->f_op->llseek)
		err = client_file->f_op->llseek(client_file, offset, origin);
	else
		err = default_llseek(client_file, offset, origin);

	chunkfs_close_cont_file(file, client_file, cont);
	return err;
}

static int
chunkfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct file *client_file;
	struct chunkfs_continuation *cont;
	int err;

	chunkfs_debug("enter\n");

	err = chunkfs_open_cont_file(file, &file->f_pos, &client_file, &cont);
	if (err)
		return err;

	err = client_file->f_op->iterate(client_file, ctx);
	/* If we read off the end, no problemo */
	if (err == -ENODATA)
		err = 0;

	chunkfs_close_cont_file(file, client_file, cont);

	return err;
}

struct file_operations chunkfs_dir_fops = {
	.llseek		= chunkfs_dir_llseek,
	.read		= generic_read_dir,
	.open		= chunkfs_open,
	.iterate	= chunkfs_iterate,
};
