// SPDX-License-Identifier: GPL-2.0
/*
 * Anonymous Shared Memory Subsystem, ashmem
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2020 Sultan Alsawaf <sultan@kerneltoast.com>
 *
 * Robert Love <rlove@google.com>
 *
 * Rewritten by Sultan Alsawaf to improve performance with fine-grained
 * locking and atomic operations, removing dead code paths.
 */

#define pr_fmt(fmt) "ashmem: " fmt

#include <linux/init.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <linux/miscdevice.h>
#include <linux/security.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/personality.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include "ashmem.h"

#define ASHMEM_NAME_PREFIX "dev/ashmem/"
#define ASHMEM_NAME_PREFIX_LEN (sizeof(ASHMEM_NAME_PREFIX) - 1)
#define ASHMEM_FULL_NAME_LEN (ASHMEM_NAME_LEN + ASHMEM_NAME_PREFIX_LEN)

/**
 * struct ashmem_area - The anonymous shared memory area
 * @name:		The optional name in /proc/pid/maps
 * @file:		The shmem-based backing file
 * @size:		The size of the mapping, in bytes
 * @prot_mask:		The allowed protection bits, as vm_flags
 * @lock:		Protects this area's fields
 *
 * The lifecycle of this structure is from our parent file's open() until
 * its release().
 */
struct ashmem_area {
	char name[ASHMEM_FULL_NAME_LEN];
	struct file *file;
	size_t size;
	unsigned long prot_mask;
	struct mutex lock;
};

/*
 * A separate lockdep class for the backing shmem inodes to resolve the lockdep
 * warning about the race between kswapd taking fs_reclaim before inode_lock
 * and write syscall taking inode_lock and then fs_reclaim.
 * Note that such race is impossible because ashmem does not support write
 * syscalls operating on the backing shmem.
 */
static struct lock_class_key backing_shmem_inode_class;

static struct kmem_cache *ashmem_area_cachep __read_mostly;

#define PROT_MASK		(PROT_EXEC | PROT_READ | PROT_WRITE)

/**
 * ashmem_open() - Opens an Anonymous Shared Memory structure
 * @inode:	   The backing file's index node
 * @file:	   The backing file
 *
 * Return: 0 if successful, or another code if unsuccessful.
 */
static int ashmem_open(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma;
	int ret;

	ret = generic_file_open(inode, file);
	if (ret)
		return ret;

	asma = kmem_cache_zalloc(ashmem_area_cachep, GFP_KERNEL);
	if (unlikely(!asma))
		return -ENOMEM;

	memcpy(asma->name, ASHMEM_NAME_PREFIX, ASHMEM_NAME_PREFIX_LEN);
	asma->prot_mask = PROT_MASK;
	mutex_init(&asma->lock);
	file->private_data = asma;

	return 0;
}

/**
 * ashmem_release() - Releases an Anonymous Shared Memory structure
 * @ignored:	      The backing file's Index Node - ignored here
 * @file:	      The backing file
 *
 * Return: 0 if successful.
 */
static int ashmem_release(struct inode *ignored, struct file *file)
{
	struct ashmem_area *asma = file->private_data;

	if (asma->file)
		fput(asma->file);
	kmem_cache_free(ashmem_area_cachep, asma);

	return 0;
}

static ssize_t ashmem_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct ashmem_area *asma = iocb->ki_filp->private_data;
	struct file *vmfile;
	ssize_t ret;

	mutex_lock(&asma->lock);

	/* If size is not set, or set to 0, always return EOF. */
	if (!asma->size) {
		ret = 0;
		goto out;
	}

	vmfile = asma->file;
	if (!vmfile) {
		ret = -EBADF;
		goto out;
	}

	/*
	 * file is guaranteed to be valid until ashmem_release, so we can
	 * safely drop the lock here.
	 */
	get_file(vmfile);
	mutex_unlock(&asma->lock);

	ret = vfs_iter_read(vmfile, iter, &iocb->ki_pos, 0);
	if (ret > 0)
		vmfile->f_pos = iocb->ki_pos;
	fput(vmfile);
	return ret;

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static loff_t ashmem_llseek(struct file *file, loff_t offset, int origin)
{
	struct ashmem_area *asma = file->private_data;
	struct file *vmfile;
	loff_t ret;

	mutex_lock(&asma->lock);

	if (!asma->size) {
		ret = -EINVAL;
		goto out;
	}

	vmfile = asma->file;
	if (!vmfile) {
		ret = -EBADF;
		goto out;
	}

	get_file(vmfile);
	mutex_unlock(&asma->lock);

	ret = vfs_llseek(vmfile, offset, origin);
	if (ret >= 0)
		file->f_pos = vmfile->f_pos;
	fput(vmfile);
	return ret;

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static inline vm_flags_t calc_vm_may_flags(unsigned long prot)
{
	return _calc_vm_trans(prot, PROT_READ,  VM_MAYREAD) |
	       _calc_vm_trans(prot, PROT_WRITE, VM_MAYWRITE) |
	       _calc_vm_trans(prot, PROT_EXEC,  VM_MAYEXEC);
}

static int ashmem_vmfile_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* do not allow to mmap ashmem backing shmem file directly */
	return -EPERM;
}

static unsigned long
ashmem_vmfile_get_unmapped_area(struct file *file, unsigned long addr,
				unsigned long len, unsigned long pgoff,
				unsigned long flags)
{
	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);
}

static int ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	static struct file_operations vmfile_fops;
	struct ashmem_area *asma = file->private_data;
	int ret = 0;

	mutex_lock(&asma->lock);

	/* user needs to SET_SIZE before mapping */
	if (unlikely(!asma->size)) {
		ret = -EINVAL;
		goto out;
	}

	/* requested mapping size larger than object size */
	if (vma->vm_end - vma->vm_start > PAGE_ALIGN(asma->size)) {
		ret = -EINVAL;
		goto out;
	}

	/* requested protection bits must match our allowed protection mask */
	if (unlikely((vma->vm_flags & ~calc_vm_prot_bits(asma->prot_mask, 0)) &
		     calc_vm_prot_bits(PROT_MASK, 0))) {
		ret = -EPERM;
		goto out;
	}
	vma->vm_flags &= ~calc_vm_may_flags(~asma->prot_mask);

	if (!asma->file) {
		char *name = ASHMEM_NAME_DEF;
		struct file *vmfile;
		struct inode *inode;

		if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0')
			name = asma->name;

		/* ... and allocate the backing shmem file */
		vmfile = shmem_file_setup(name, asma->size, vma->vm_flags);
		if (IS_ERR(vmfile)) {
			ret = PTR_ERR(vmfile);
			goto out;
		}
		vmfile->f_mode |= FMODE_LSEEK;
		inode = file_inode(vmfile);
		lockdep_set_class(&inode->i_rwsem, &backing_shmem_inode_class);
		asma->file = vmfile;
		/*
		 * override mmap operation of the vmfile so that it can't be
		 * remapped which would lead to creation of a new vma with no
		 * asma permission checks. Have to override get_unmapped_area
		 * as well to prevent VM_BUG_ON check for f_ops modification.
		 */
		if (!vmfile_fops.mmap) {
			vmfile_fops = *vmfile->f_op;
			vmfile_fops.mmap = ashmem_vmfile_mmap;
			vmfile_fops.get_unmapped_area =
					ashmem_vmfile_get_unmapped_area;
		}
		vmfile->f_op = &vmfile_fops;
	}
	get_file(asma->file);

	if (vma->vm_flags & VM_SHARED)
		shmem_set_file(vma, asma->file);
	else {
		if (vma->vm_file)
			fput(vma->vm_file);
		vma->vm_file = asma->file;
	}

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static int set_prot_mask(struct ashmem_area *asma, unsigned long prot)
{
	int ret = 0;

	mutex_lock(&asma->lock);

	/* the user can only remove, not add, protection bits */
	if (unlikely((asma->prot_mask & prot) != prot)) {
		ret = -EINVAL;
		goto out;
	}

	/* does the application expect PROT_READ to imply PROT_EXEC? */
	if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
		prot |= PROT_EXEC;

	asma->prot_mask = prot;

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static int set_name(struct ashmem_area *asma, void __user *name)
{
	char lname[ASHMEM_NAME_LEN];
	int len, ret = 0;

	len = strncpy_from_user(lname, name, ASHMEM_NAME_LEN);
	if (len < 0)
		return len;
	if (len == ASHMEM_NAME_LEN)
		lname[ASHMEM_NAME_LEN - 1] = '\0';

	mutex_lock(&asma->lock);
	/* cannot change an existing mapping's name */
	if (unlikely(asma->file))
		ret = -EINVAL;
	else
		strcpy(asma->name + ASHMEM_NAME_PREFIX_LEN, lname);
	mutex_unlock(&asma->lock);

	return ret;
}

static int get_name(struct ashmem_area *asma, void __user *name)
{
	char lname[ASHMEM_NAME_LEN];
	size_t len;

	mutex_lock(&asma->lock);
	if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0') {
		len = strlen(asma->name + ASHMEM_NAME_PREFIX_LEN) + 1;
		memcpy(lname, asma->name + ASHMEM_NAME_PREFIX_LEN, len);
	} else {
		len = sizeof(ASHMEM_NAME_DEF);
		memcpy(lname, ASHMEM_NAME_DEF, len);
	}
	mutex_unlock(&asma->lock);

	if (copy_to_user(name, lname, len))
		return -EFAULT;
	return 0;
}

static long ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ashmem_area *asma = file->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case ASHMEM_SET_NAME:
		ret = set_name(asma, (void __user *)arg);
		break;
	case ASHMEM_GET_NAME:
		ret = get_name(asma, (void __user *)arg);
		break;
	case ASHMEM_SET_SIZE:
		mutex_lock(&asma->lock);
		if (unlikely(asma->file)) {
			ret = -EINVAL;
		} else {
			ret = 0;
			asma->size = (size_t)arg;
		}
		mutex_unlock(&asma->lock);
		break;
	case ASHMEM_GET_SIZE:
		ret = asma->size;
		break;
	case ASHMEM_SET_PROT_MASK:
		ret = set_prot_mask(asma, arg);
		break;
	case ASHMEM_GET_PROT_MASK:
		ret = asma->prot_mask;
		break;
	case ASHMEM_PIN:
	case ASHMEM_UNPIN:
		ret = ASHMEM_NOT_PURGED;
		break;
	case ASHMEM_GET_PIN_STATUS:
		ret = ASHMEM_IS_PINNED;
		break;
	case ASHMEM_PURGE_ALL_CACHES:
		ret = capable(CAP_SYS_ADMIN) ? 0 : -EPERM;
		break;
	}

	return ret;
}

/* support of 32bit userspace on 64bit platforms */
#ifdef CONFIG_COMPAT
static long compat_ashmem_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	switch (cmd) {
	case COMPAT_ASHMEM_SET_SIZE:
		cmd = ASHMEM_SET_SIZE;
		break;
	case COMPAT_ASHMEM_SET_PROT_MASK:
		cmd = ASHMEM_SET_PROT_MASK;
		break;
	}
	return ashmem_ioctl(file, cmd, arg);
}
#endif

static const struct file_operations ashmem_fops = {
	.owner = THIS_MODULE,
	.open = ashmem_open,
	.release = ashmem_release,
	.read_iter = ashmem_read_iter,
	.llseek = ashmem_llseek,
	.mmap = ashmem_mmap,
	.unlocked_ioctl = ashmem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ashmem_ioctl,
#endif
};

static struct miscdevice ashmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ashmem",
	.fops = &ashmem_fops,
};

static int __init ashmem_init(void)
{
	int ret = -ENOMEM;

	ashmem_area_cachep = kmem_cache_create("ashmem_area_cache",
					       sizeof(struct ashmem_area),
					       0, 0, NULL);
	if (unlikely(!ashmem_area_cachep)) {
		pr_err("failed to create slab cache\n");
		goto out;
	}

	ret = misc_register(&ashmem_misc);
	if (unlikely(ret)) {
		pr_err("failed to register misc device!\n");
		goto out_free1;
	}

	pr_info("initialized\n");

	return 0;

out_free1:
	kmem_cache_destroy(ashmem_area_cachep);
out:
	return ret;
}
device_initcall(ashmem_init);
