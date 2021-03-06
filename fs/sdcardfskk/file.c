/*
 * fs/sdcardfskk/file.c
 *
 * Copyright (c) 2013 Samsung Electronics Co. Ltd
 *   Authors: Daeho Jeong, Woojoong Lee, Seunghwan Hyun,
 *               Sunghwan Yun, Sungjong Seo
 *
 * This program has been developed as a stackable file system based on
 * the WrapFS which written by
 *
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009     Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This file is dual licensed.  It may be redistributed and/or modified
 * under the terms of the Apache 2.0 License OR version 2 of the GNU
 * General Public License.
 */

#include "sdcardfskk.h"
#ifdef CONFIG_SDCARD_FS_FADV_NOACTIVE
#include <linux/backing-dev.h>
#endif

static ssize_t sdcardfskk_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	int err;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;
#ifdef CONFIG_SDCARD_FS_FADV_NOACTIVE
	struct backing_dev_info *bdi;
#endif

	lower_file = sdcardfskk_lower_file(file);

#ifdef CONFIG_SDCARD_FS_FADV_NOACTIVE
	if (file->f_mode & FMODE_NOACTIVE) {
		if (!(lower_file->f_mode & FMODE_NOACTIVE)) {
			bdi = lower_file->f_mapping->backing_dev_info;
			lower_file->f_ra.ra_pages = bdi->ra_pages * 2;
			spin_lock(&lower_file->f_lock);
			lower_file->f_mode |= FMODE_NOACTIVE;
			spin_unlock(&lower_file->f_lock);
		}
	}
#endif

	err = vfs_read(lower_file, buf, count, ppos);
	/* update our inode atime upon a successful lower read */
	if (err >= 0)
		fsstack_copy_attr_atime(dentry->d_inode,
					lower_file->f_path.dentry->d_inode);

	return err;
}

static ssize_t sdcardfskk_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	int err = 0;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;

	/* check disk space */
	if (!check_min_free_space(dentry, count, 0)) {
		printk(KERN_INFO "No minimum free space.\n");
		return -ENOSPC;
	}

	lower_file = sdcardfskk_lower_file(file);
	err = vfs_write(lower_file, buf, count, ppos);
	/* update our inode times+sizes upon a successful lower write */
	if (err >= 0) {
		fsstack_copy_inode_size(dentry->d_inode,
					lower_file->f_path.dentry->d_inode);
		fsstack_copy_attr_times(dentry->d_inode,
					lower_file->f_path.dentry->d_inode);
	}

	return err;
}

static int sdcardfskk_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = sdcardfskk_lower_file(file);

	lower_file->f_pos = file->f_pos;
	err = vfs_readdir(lower_file, filldir, dirent);
	file->f_pos = lower_file->f_pos;
	if (err >= 0)		/* copy the atime */
		fsstack_copy_attr_atime(dentry->d_inode,
					lower_file->f_path.dentry->d_inode);
	return err;
}

static long sdcardfskk_unlocked_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = sdcardfskk_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

out:
	return err;
}

#ifdef CONFIG_COMPAT
static long sdcardfskk_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = sdcardfskk_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

out:
	return err;
}
#endif

static int sdcardfskk_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	bool willwrite;
	struct file *lower_file;
	const struct vm_operations_struct *saved_vm_ops = NULL;

	/* this might be deferred to mmap's writepage */
	willwrite = ((vma->vm_flags | VM_SHARED | VM_WRITE) == vma->vm_flags);

	/*
	 * File systems which do not implement ->writepage may use
	 * generic_file_readonly_mmap as their ->mmap op.  If you call
	 * generic_file_readonly_mmap with VM_WRITE, you'd get an -EINVAL.
	 * But we cannot call the lower ->mmap op, so we can't tell that
	 * writeable mappings won't work.  Therefore, our only choice is to
	 * check if the lower file system supports the ->writepage, and if
	 * not, return EINVAL (the same error that
	 * generic_file_readonly_mmap returns in that case).
	 */
	lower_file = sdcardfskk_lower_file(file);
	if (willwrite && !lower_file->f_mapping->a_ops->writepage) {
		err = -EINVAL;
		printk(KERN_ERR "sdcardfskk: lower file system does not "
		       "support writeable mmap\n");
		goto out;
	}

	/*
	 * find and save lower vm_ops.
	 *
	 * XXX: the VFS should have a cleaner way of finding the lower vm_ops
	 */
	if (!SDCARDFSKK_F(file)->lower_vm_ops) {
		err = lower_file->f_op->mmap(lower_file, vma);
		if (err) {
			printk(KERN_ERR "sdcardfskk: lower mmap failed %d\n", err);
			goto out;
		}
		saved_vm_ops = vma->vm_ops; /* save: came from lower ->mmap */
		err = do_munmap(current->mm, vma->vm_start,
				vma->vm_end - vma->vm_start);
		if (err) {
			printk(KERN_ERR "sdcardfskk: do_munmap failed %d\n", err);
			goto out;
		}
	}

	/*
	 * Next 3 lines are all I need from generic_file_mmap.  I definitely
	 * don't want its test for ->readpage which returns -ENOEXEC.
	 */
	file_accessed(file);
	vma->vm_ops = &sdcardfskk_vm_ops;
	vma->vm_flags |= VM_CAN_NONLINEAR;

	file->f_mapping->a_ops = &sdcardfskk_aops; /* set our aops */
	if (!SDCARDFSKK_F(file)->lower_vm_ops) /* save for our ->fault */
		SDCARDFSKK_F(file)->lower_vm_ops = saved_vm_ops;

out:
	return err;
}

static int sdcardfskk_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;
	struct dentry *parent = dget_parent(dentry);
	struct sdcardfskk_sb_info *sbi = SDCARDFSKK_SB(dentry->d_sb);
	const struct cred *saved_cred = NULL;
	int has_rw;

	/* don't open unhashed/deleted files */
	if (d_unhashed(dentry)) {
		err = -ENOENT;
		goto out_err;
	}

	has_rw = get_caller_has_rw_locked_kitkat(sbi->pkgl_id, sbi->options.derive);

	if(!check_caller_access_to_name_kitkat(parent->d_inode, dentry->d_name.name,
				sbi->options.derive,
				open_flags_to_access_mode_kitkat(file->f_flags), has_rw)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
                         "	dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_err;
	}

	/* save current_cred and override it */
	OVERRIDE_CRED(sbi, saved_cred);

	file->private_data =
		kzalloc(sizeof(struct sdcardfskk_file_info), GFP_KERNEL);
	if (!SDCARDFSKK_F(file)) {
		err = -ENOMEM;
		goto out_revert_cred;
	}

	/* open lower object and link sdcardfskk's file struct to lower's */
	sdcardfskk_get_lower_path(file->f_path.dentry, &lower_path);
	lower_file = dentry_open(lower_path.dentry, lower_path.mnt,
				 file->f_flags, current_cred());
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		lower_file = sdcardfskk_lower_file(file);
		if (lower_file) {
			sdcardfskk_set_lower_file(file, NULL);
			fput(lower_file); /* fput calls dput for lower_dentry */
		}
	} else {
		sdcardfskk_set_lower_file(file, lower_file);
	}

	if (err)
		kfree(SDCARDFSKK_F(file));
	else {
		mutex_lock(&inode->i_mutex);
		sdcardfskk_copy_inode_attr(inode, sdcardfskk_lower_inode(inode));
		fix_derived_permission(inode);
		mutex_unlock(&inode->i_mutex);
	}

out_revert_cred:
	REVERT_CRED(saved_cred);
out_err:
	dput(parent);
	return err;
}

static int sdcardfskk_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = sdcardfskk_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush)
		err = lower_file->f_op->flush(lower_file, id);

	return err;
}

/* release all lower object references & free the file info structure */
static int sdcardfskk_file_release(struct inode *inode, struct file *file)
{
	struct file *lower_file;

	lower_file = sdcardfskk_lower_file(file);
	if (lower_file) {
		sdcardfskk_set_lower_file(file, NULL);
		fput(lower_file);
	}

	kfree(SDCARDFSKK_F(file));
	return 0;
}

static int
sdcardfskk_fsync(struct file *file, int datasync)
{
	int err;
	struct file *lower_file;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = sdcardfskk_lower_file(file);
	sdcardfskk_get_lower_path(dentry, &lower_path);
	err = vfs_fsync(lower_file, datasync);
	sdcardfskk_put_lower_path(dentry, &lower_path);

	return err;
}

static int sdcardfskk_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = sdcardfskk_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);

	return err;
}

const struct file_operations sdcardfskk_main_fops = {
	.llseek		= generic_file_llseek,
	.read		= sdcardfskk_read,
	.write		= sdcardfskk_write,
	.unlocked_ioctl	= sdcardfskk_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sdcardfskk_compat_ioctl,
#endif
	.mmap		= sdcardfskk_mmap,
	.open		= sdcardfskk_open,
	.flush		= sdcardfskk_flush,
	.release	= sdcardfskk_file_release,
	.fsync		= sdcardfskk_fsync,
	.fasync		= sdcardfskk_fasync,
};

/* trimmed directory options */
const struct file_operations sdcardfskk_dir_fops = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.readdir	= sdcardfskk_readdir,
	.unlocked_ioctl	= sdcardfskk_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sdcardfskk_compat_ioctl,
#endif
	.open		= sdcardfskk_open,
	.release	= sdcardfskk_file_release,
	.flush		= sdcardfskk_flush,
	.fsync		= sdcardfskk_fsync,
	.fasync		= sdcardfskk_fasync,
};
