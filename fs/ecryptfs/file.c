/**
 * eCryptfs: Linux filesystem encryption layer
 *
 * Copyright (C) 1997-2004 Erez Zadok
 * Copyright (C) 2001-2004 Stony Brook University
 * Copyright (C) 2004-2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mhalcrow@us.ibm.com>
 *   		Michael C. Thompson <mcthomps@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <linux/file.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/security.h>
#include <linux/compat.h>
#include <linux/fs_stack.h>
#include "ecryptfs_kernel.h"

#ifdef CONFIG_WTL_ENCRYPTION_FILTER
#include <linux/ctype.h>
#define ECRYPTFS_IOCTL_GET_ATTRIBUTES	_IOR('l', 0x10, __u32)
#define ECRYPTFS_WAS_ENCRYPTED 128
#endif


/**
 * ecryptfs_read_update_atime
 *
 * generic_file_read updates the atime of upper layer inode.  But, it
 * doesn't give us a chance to update the atime of the lower layer
 * inode.  This function is a wrapper to generic_file_read.  It
 * updates the atime of the lower level inode if generic_file_read
 * returns without any errors. This is to be used only for file reads.
 * The function to be used for directory reads is ecryptfs_read.
 */
static ssize_t ecryptfs_read_update_atime(struct kiocb *iocb,
				const struct iovec *iov,
				unsigned long nr_segs, loff_t pos)
{
	ssize_t rc;
	struct dentry *lower_dentry;
	struct vfsmount *lower_vfsmount;
	struct file *file = iocb->ki_filp;

	rc = generic_file_aio_read(iocb, iov, nr_segs, pos);
	/*
	 * Even though this is a async interface, we need to wait
	 * for IO to finish to update atime
	 */
	if (-EIOCBQUEUED == rc)
		rc = wait_on_sync_kiocb(iocb);
	if (rc >= 0) {
		lower_dentry = ecryptfs_dentry_to_lower(file->f_path.dentry);
		lower_vfsmount = ecryptfs_dentry_to_lower_mnt(file->f_path.dentry);
		touch_atime(lower_vfsmount, lower_dentry);
	}
	return rc;
}

struct ecryptfs_getdents_callback {
	void *dirent;
	struct dentry *dentry;
	filldir_t filldir;
	int filldir_called;
	int entries_written;
};

/* Inspired by generic filldir in fs/readdir.c */
static int
ecryptfs_filldir(void *dirent, const char *lower_name, int lower_namelen,
		 loff_t offset, u64 ino, unsigned int d_type)
{
	struct ecryptfs_getdents_callback *buf =
	    (struct ecryptfs_getdents_callback *)dirent;
	size_t name_size;
	char *name;
	int rc;

	buf->filldir_called++;
	rc = ecryptfs_decode_and_decrypt_filename(&name, &name_size,
						  buf->dentry, lower_name,
						  lower_namelen);
	if (rc) {
		printk(KERN_ERR "%s: Error attempting to decode and decrypt "
		       "filename [%s]; rc = [%d]\n", __func__, lower_name,
		       rc);
		goto out;
	}
	rc = buf->filldir(buf->dirent, name, name_size, offset, ino, d_type);
	kfree(name);
	if (rc >= 0)
		buf->entries_written++;
out:
	return rc;
}

/**
 * ecryptfs_readdir
 * @file: The eCryptfs directory file
 * @dirent: Directory entry handle
 * @filldir: The filldir callback function
 */
static int ecryptfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	int rc;
	struct file *lower_file;
	struct inode *inode;
	struct ecryptfs_getdents_callback buf;

	lower_file = ecryptfs_file_to_lower(file);
	lower_file->f_pos = file->f_pos;
	inode = file->f_path.dentry->d_inode;
	memset(&buf, 0, sizeof(buf));
	buf.dirent = dirent;
	buf.dentry = file->f_path.dentry;
	buf.filldir = filldir;
	buf.filldir_called = 0;
	buf.entries_written = 0;
	rc = vfs_readdir(lower_file, ecryptfs_filldir, (void *)&buf);
	file->f_pos = lower_file->f_pos;
	if (rc < 0)
		goto out;
	if (buf.filldir_called && !buf.entries_written)
		goto out;
	if (rc >= 0)
		fsstack_copy_attr_atime(inode,
					lower_file->f_path.dentry->d_inode);
out:
	return rc;
}

static void ecryptfs_vma_close(struct vm_area_struct *vma)
{
	filemap_write_and_wait(vma->vm_file->f_mapping);
}

static const struct vm_operations_struct ecryptfs_file_vm_ops = {
	.close		= ecryptfs_vma_close,
	.fault		= filemap_fault,
};

static int ecryptfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	int rc;
	struct file *lower_file = ecryptfs_file_to_lower(file);

	/*
	 * Don't allow mmap on top of file systems that don't support it
	 * natively.  If FILESYSTEM_MAX_STACK_DEPTH > 2 or ecryptfs
	 * allows recursive mounting, this will need to be extended.
	 */
	if (!lower_file->f_op->mmap)
		return -ENODEV;

	rc = generic_file_mmap(file, vma);
	if (!rc)
		vma->vm_ops = &ecryptfs_file_vm_ops;

	return rc;
}

struct kmem_cache *ecryptfs_file_info_cache;

/**
 * ecryptfs_open
 * @inode: inode speciying file to open
 * @file: Structure to return filled in
 *
 * Opens the file specified by inode.
 *
 * Returns zero on success; non-zero otherwise
 */
static int ecryptfs_open(struct inode *inode, struct file *file)
{
	int rc = 0;
	struct ecryptfs_crypt_stat *crypt_stat = NULL;
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat;
	struct dentry *ecryptfs_dentry = file->f_path.dentry;
	/* Private value of ecryptfs_dentry allocated in
	 * ecryptfs_lookup() */
	struct dentry *lower_dentry;
	struct ecryptfs_file_info *file_info;

	mount_crypt_stat = &ecryptfs_superblock_to_private(
		ecryptfs_dentry->d_sb)->mount_crypt_stat;
	if ((mount_crypt_stat->flags & ECRYPTFS_ENCRYPTED_VIEW_ENABLED)
	    && ((file->f_flags & O_WRONLY) || (file->f_flags & O_RDWR)
		|| (file->f_flags & O_CREAT) || (file->f_flags & O_TRUNC)
		|| (file->f_flags & O_APPEND))) {
		printk(KERN_WARNING "Mount has encrypted view enabled; "
		       "files may only be read\n");
		rc = -EPERM;
		goto out;
	}
	/* Released in ecryptfs_release or end of function if failure */
	file_info = kmem_cache_zalloc(ecryptfs_file_info_cache, GFP_KERNEL);
	ecryptfs_set_file_private(file, file_info);
	if (!file_info) {
		ecryptfs_printk(KERN_ERR,
				"Error attempting to allocate memory\n");
		rc = -ENOMEM;
		goto out;
	}
	lower_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry);
	crypt_stat = &ecryptfs_inode_to_private(inode)->crypt_stat;
	mutex_lock(&crypt_stat->cs_mutex);
	if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED)) {
		ecryptfs_printk(KERN_DEBUG, "Setting flags for stat...\n");
		/* Policy code enabled in future release */
		crypt_stat->flags |= (ECRYPTFS_POLICY_APPLIED
				      | ECRYPTFS_ENCRYPTED);
	}
	mutex_unlock(&crypt_stat->cs_mutex);
	rc = ecryptfs_get_lower_file(ecryptfs_dentry, inode);
	if (rc) {
		printk(KERN_ERR "%s: Error attempting to initialize "
			"the lower file for the dentry with name "
			"[%s]; rc = [%d]\n", __func__,
			ecryptfs_dentry->d_name.name, rc);
		goto out_free;
	}
	if ((ecryptfs_inode_to_private(inode)->lower_file->f_flags & O_ACCMODE)
	    == O_RDONLY && (file->f_flags & O_ACCMODE) != O_RDONLY) {
		rc = -EPERM;
		printk(KERN_WARNING "%s: Lower file is RO; eCryptfs "
		       "file must hence be opened RO\n", __func__);
		goto out_put;
	}
	ecryptfs_set_file_lower(
		file, ecryptfs_inode_to_private(inode)->lower_file);
	if (S_ISDIR(ecryptfs_dentry->d_inode->i_mode)) {
		ecryptfs_printk(KERN_DEBUG, "This is a directory\n");
		mutex_lock(&crypt_stat->cs_mutex);
		crypt_stat->flags &= ~(ECRYPTFS_ENCRYPTED);
		mutex_unlock(&crypt_stat->cs_mutex);
		rc = 0;
		goto out;
	}
#ifdef CONFIG_WTL_ENCRYPTION_FILTER
	mutex_lock(&crypt_stat->cs_mutex);
	if ((mount_crypt_stat->flags & ECRYPTFS_ENABLE_NEW_PASSTHROUGH)
			&& (crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
		if (ecryptfs_read_metadata(ecryptfs_dentry)) {
			crypt_stat->flags &= ~(ECRYPTFS_I_SIZE_INITIALIZED
					       | ECRYPTFS_ENCRYPTED);
			mutex_unlock(&crypt_stat->cs_mutex);
			goto out;
		}
	} else if ((mount_crypt_stat->flags & ECRYPTFS_ENABLE_FILTERING)
			&& (crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
		struct dentry *fp_dentry =
			ecryptfs_inode_to_private(inode)->lower_file->f_dentry;
		char filename[NAME_MAX+1] = {0};
		if (fp_dentry->d_name.len <= NAME_MAX)
			memcpy(filename, fp_dentry->d_name.name,
					fp_dentry->d_name.len + 1);

		if (is_file_name_match(mount_crypt_stat, fp_dentry)
			|| is_file_ext_match(mount_crypt_stat, filename)) {
			if (ecryptfs_read_metadata(ecryptfs_dentry))
				crypt_stat->flags &=
				~(ECRYPTFS_I_SIZE_INITIALIZED
				| ECRYPTFS_ENCRYPTED);
			mutex_unlock(&crypt_stat->cs_mutex);
			goto out;
		}
	}
	mutex_unlock(&crypt_stat->cs_mutex);
#endif
	mutex_lock(&crypt_stat->cs_mutex);
	if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED)
	    || !(crypt_stat->flags & ECRYPTFS_KEY_VALID)) {
		rc = ecryptfs_read_metadata(ecryptfs_dentry);
		if (rc) {
			ecryptfs_printk(KERN_DEBUG,
					"Valid headers not found\n");
			if (!(mount_crypt_stat->flags
			      & ECRYPTFS_PLAINTEXT_PASSTHROUGH_ENABLED)) {
				rc = -EIO;
				printk(KERN_WARNING "Either the lower file "
				       "is not in a valid eCryptfs format, "
				       "or the key could not be retrieved. "
				       "Plaintext passthrough mode is not "
				       "enabled; returning -EIO\n");
				mutex_unlock(&crypt_stat->cs_mutex);
				goto out_put;
			}
			rc = 0;
			crypt_stat->flags &= ~(ECRYPTFS_I_SIZE_INITIALIZED
					       | ECRYPTFS_ENCRYPTED);
			mutex_unlock(&crypt_stat->cs_mutex);
			goto out;
		}
	}
	mutex_unlock(&crypt_stat->cs_mutex);
	ecryptfs_printk(KERN_DEBUG, "inode w/ addr = [0x%p], i_ino = "
			"[0x%.16lx] size: [0x%.16llx]\n", inode, inode->i_ino,
			(unsigned long long)i_size_read(inode));
	goto out;
out_put:
	ecryptfs_put_lower_file(inode);
out_free:
	kmem_cache_free(ecryptfs_file_info_cache,
			ecryptfs_file_to_private(file));
out:
	return rc;
}

static int ecryptfs_flush(struct file *file, fl_owner_t td)
{
	return file->f_mode & FMODE_WRITE
	       ? filemap_write_and_wait(file->f_mapping) : 0;
}

static int ecryptfs_release(struct inode *inode, struct file *file)
{
	ecryptfs_put_lower_file(inode);
	kmem_cache_free(ecryptfs_file_info_cache,
			ecryptfs_file_to_private(file));
	return 0;
}

static int
ecryptfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	int rc = 0;

	rc = generic_file_fsync(file, start, end, datasync);
	if (rc)
		goto out;
	rc = vfs_fsync_range(ecryptfs_file_to_lower(file), start, end,
			     datasync);
out:
	return rc;
}

static int ecryptfs_fasync(int fd, struct file *file, int flag)
{
	int rc = 0;
	struct file *lower_file = NULL;

	lower_file = ecryptfs_file_to_lower(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		rc = lower_file->f_op->fasync(fd, lower_file, flag);
	return rc;
}

static long
ecryptfs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file *lower_file = NULL;
	long rc = -ENOTTY;
#ifdef CONFIG_WTL_ENCRYPTION_FILTER
	if (cmd == ECRYPTFS_IOCTL_GET_ATTRIBUTES) {
		u32 __user *user_attr = (u32 __user *)arg;
		u32 attr = 0;
		char filename[NAME_MAX+1] = {0};
		struct dentry *ecryptfs_dentry = file->f_path.dentry;
		struct ecryptfs_mount_crypt_stat *mount_crypt_stat =
			&ecryptfs_superblock_to_private(ecryptfs_dentry->d_sb)
				->mount_crypt_stat;

		struct inode *inode = ecryptfs_dentry->d_inode;
		struct ecryptfs_crypt_stat *crypt_stat =
			&ecryptfs_inode_to_private(inode)->crypt_stat;
		struct dentry *fp_dentry =
			ecryptfs_inode_to_private(inode)->lower_file->f_dentry;
		if (fp_dentry->d_name.len <= NAME_MAX)
			memcpy(filename, fp_dentry->d_name.name,
					fp_dentry->d_name.len + 1);

		mutex_lock(&crypt_stat->cs_mutex);
		if ((crypt_stat->flags & ECRYPTFS_ENCRYPTED) ||
			((mount_crypt_stat->flags & ECRYPTFS_ENABLE_FILTERING)
			&& (is_file_name_match(mount_crypt_stat, fp_dentry)
			|| is_file_ext_match(mount_crypt_stat, filename))))
			attr = ECRYPTFS_WAS_ENCRYPTED;
		mutex_unlock(&crypt_stat->cs_mutex);
		put_user(attr, user_attr);
		return 0;
	}
#endif
	if (ecryptfs_file_to_private(file))
		lower_file = ecryptfs_file_to_lower(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->unlocked_ioctl)
		rc = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);
	return rc;
}

#ifdef CONFIG_COMPAT
static long
ecryptfs_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file *lower_file = NULL;
	long rc = -ENOIOCTLCMD;

	if (ecryptfs_file_to_private(file))
		lower_file = ecryptfs_file_to_lower(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->compat_ioctl)
		rc = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);
	return rc;
}
#endif

#ifdef CONFIG_WTL_ENCRYPTION_FILTER
int is_file_name_match(struct ecryptfs_mount_crypt_stat *mcs,
					struct dentry *fp_dentry)
{
	int i;
	char *str = NULL;
	if (!(strcmp("/", fp_dentry->d_name.name))
		|| !(strcmp("", fp_dentry->d_name.name)))
		return 0;
	str = kzalloc(mcs->max_name_filter_len + 1, GFP_KERNEL);
	if (!str) {
		printk(KERN_ERR "%s: Out of memory whilst attempting "
			       "to kzalloc [%zd] bytes\n", __func__,
			       (mcs->max_name_filter_len + 1));
		return 0;
	}

	for (i = 0; i < ENC_NAME_FILTER_MAX_INSTANCE; i++) {
		int len = 0;
		struct dentry *p = fp_dentry;
		if (!mcs->enc_filter_name[i] ||
			 !strlen(mcs->enc_filter_name[i]))
			break;

		while (1) {
			if (len == 0) {
				len = strlen(p->d_name.name);
				if (len > mcs->max_name_filter_len)
					break;
				strcpy(str, p->d_name.name);
			} else {
				len = len + 1 + strlen(p->d_name.name) ;
				if (len > mcs->max_name_filter_len)
					break;
				strcat(str, "/");
				strcat(str, p->d_name.name);
			}

			if (strnicmp(str, mcs->enc_filter_name[i], len))
				break;
			p = p->d_parent;

			if (!(strcmp("/", p->d_name.name))
				|| !(strcmp("", p->d_name.name))) {
				if (len == strlen(mcs->enc_filter_name[i])) {
					kfree(str);
					return 1;
				}
				break;
			}
		}
	}
	kfree(str);
	return 0;
}

int is_file_ext_match(struct ecryptfs_mount_crypt_stat *mcs, char *str)
{
	int i;
	char ext[NAME_MAX + 1] = {0};

	char *token;
	int count = 0;
	while ((token = strsep(&str, ".")) != NULL) {
		strncpy(ext, token, NAME_MAX);
		count++;
	}
	if (count <= 1)
		return 0;

	for (i = 0; i < ENC_EXT_FILTER_MAX_INSTANCE; i++) {
		if (!mcs->enc_filter_ext[i] || !strlen(mcs->enc_filter_ext[i]))
			return 0;
		if (strlen(ext) != strlen(mcs->enc_filter_ext[i]))
			continue;
		if (!strnicmp(ext, mcs->enc_filter_ext[i], strlen(ext)))
			return 1;
	}
	return 0;
}
#endif

const struct file_operations ecryptfs_dir_fops = {
	.readdir = ecryptfs_readdir,
	.read = generic_read_dir,
	.unlocked_ioctl = ecryptfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ecryptfs_compat_ioctl,
#endif
	.open = ecryptfs_open,
	.flush = ecryptfs_flush,
	.release = ecryptfs_release,
	.fsync = ecryptfs_fsync,
	.fasync = ecryptfs_fasync,
	.splice_read = generic_file_splice_read,
	.llseek = default_llseek,
};

const struct file_operations ecryptfs_main_fops = {
	.llseek = generic_file_llseek,
	.read = do_sync_read,
	.aio_read = ecryptfs_read_update_atime,
	.write = do_sync_write,
	.aio_write = generic_file_aio_write,
	.readdir = ecryptfs_readdir,
	.unlocked_ioctl = ecryptfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ecryptfs_compat_ioctl,
#endif
	.mmap = ecryptfs_file_mmap,
	.open = ecryptfs_open,
	.flush = ecryptfs_flush,
	.release = ecryptfs_release,
	.fsync = ecryptfs_fsync,
	.fasync = ecryptfs_fasync,
	.splice_read = generic_file_splice_read,
};
