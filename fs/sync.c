
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/backing-dev.h>
#include "internal.h"

#include <trace/events/mmcio.h>
#ifdef CONFIG_DYNAMIC_FSYNC
extern bool early_suspend_active;
extern bool dyn_fsync_active;
#endif

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)

static int __sync_filesystem(struct super_block *sb, int wait)
{
	if (sb->s_bdi == &noop_backing_dev_info)
		return 0;

	if (sb->s_qcop && sb->s_qcop->quota_sync)
		sb->s_qcop->quota_sync(sb, -1, wait);

	if (wait)
		sync_inodes_sb(sb);
	else
		writeback_inodes_sb(sb, WB_REASON_SYNC);

	if (sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, wait);
	return __sync_blockdev(sb->s_bdev, wait);
}

int sync_filesystem(struct super_block *sb)
{
	int ret;

	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	if (sb->s_flags & MS_RDONLY)
		return 0;

	if (atomic_read(&vfs_emergency_remount)) {
		pr_info("%s: force sync fs in wait mode\n", __func__);
		ret = __sync_filesystem(sb, 1);
	} else
		ret = __sync_filesystem(sb, 0);
	if (ret < 0)
		return ret;
	return __sync_filesystem(sb, 1);
}
EXPORT_SYMBOL_GPL(sync_filesystem);

static void sync_one_sb(struct super_block *sb, void *arg)
{
	if (!(sb->s_flags & MS_RDONLY))
		__sync_filesystem(sb, *(int *)arg);
}
/*
 * Sync all the data for all the filesystems (called by sys_sync() and
 * emergency sync)
 */
void sync_filesystems(int wait)
{
	iterate_supers(sync_one_sb, &wait);
}

static void do_sync(void)
{
	trace_sys_sync(0);
	wakeup_flusher_threads(0, WB_REASON_SYNC);
	sync_filesystems(0);
	sync_filesystems(1);
	if (unlikely(laptop_mode))
		laptop_sync_completion();
	trace_sys_sync_done(0);
	return;
}

static DEFINE_MUTEX(sync_mutex);	/* One do_sync() at a time. */
static unsigned long sync_seq;		/* Many sync()s from one do_sync(). */
					/*  Overflow harmless, extra wait. */

/*
 * Only allow one task to do sync() at a time, and further allow
 * concurrent sync() calls to be satisfied by a single do_sync()
 * invocation.
 */
SYSCALL_DEFINE0(sync)
{
	unsigned long snap;
	unsigned long snap_done;

	snap = ACCESS_ONCE(sync_seq);
	smp_mb();  /* Prevent above from bleeding into critical section. */
	mutex_lock(&sync_mutex);
	snap_done = sync_seq;

	/*
	 * If the value in snap is odd, we need to wait for the current
	 * do_sync() to complete, then wait for the next one, in other
	 * words, we need the value of snap_done to be three larger than
	 * the value of snap.  On the other hand, if the value in snap is
	 * even, we only have to wait for the next request to complete,
	 * in other words, we need the value of snap_done to be only two
	 * greater than the value of snap.  The "(snap + 3) & 0x1" computes
	 * this for us (thank you, Linus!).
	 */
	if (ULONG_CMP_GE(snap_done, (snap + 3) & ~0x1)) {
		/*
		 * A full do_sync() executed between our two fetches from
		 * sync_seq, so our work is done!
		 */
		smp_mb(); /* Order test with caller's subsequent code. */
		mutex_unlock(&sync_mutex);
		return 0;
	}

	/* Record the start of do_sync(). */
	ACCESS_ONCE(sync_seq)++;
	WARN_ON_ONCE((sync_seq & 0x1) != 1);
	smp_mb(); /* Keep prior increment out of do_sync(). */

	do_sync();

	/* Record the end of do_sync(). */
	smp_mb(); /* Keep subsequent increment out of do_sync(). */
	ACCESS_ONCE(sync_seq)++;
	WARN_ON_ONCE((sync_seq & 0x1) != 0);
	mutex_unlock(&sync_mutex);
	return 0;
}

static void do_sync_work(struct work_struct *work)
{
	sync_filesystems(0);
	sync_filesystems(0);
	printk("Emergency Sync complete\n");
	kfree(work);
}

void emergency_sync(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_sync_work);
		schedule_work(work);
	}
}

SYSCALL_DEFINE1(syncfs, int, fd)
{
	struct file *file;
	struct super_block *sb;
	int ret;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (!file)
		return -EBADF;
	sb = file->f_dentry->d_sb;

	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);

	fput_light(file, fput_needed);
	return ret;
}

/**
 * vfs_fsync_range - helper to sync a range of data & metadata to disk
 * @file:		file to sync
 * @start:		offset in bytes of the beginning of data range to sync
 * @end:		offset in bytes of the end of data range (inclusive)
 * @datasync:		perform only datasync
 *
 * Write back data in range @start..@end and metadata for @file to disk.  If
 * @datasync is set only metadata needed to access modified file data is
 * written.
 */
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	int err;
#ifdef CONFIG_DYNAMIC_FSYNC
	if (likely(dyn_fsync_active && !early_suspend_active))
		return 0;
	else {
#endif
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	trace_vfs_fsync(file);
	err = file->f_op->fsync(file, start, end, datasync);
	trace_vfs_fsync_done(file);
	return err;
#ifdef CONFIG_DYNAMIC_FSYNC
	}
#endif
}
EXPORT_SYMBOL(vfs_fsync_range);

/**
 * vfs_fsync - perform a fsync or fdatasync on a file
 * @file:		file to sync
 * @datasync:		only perform a fdatasync operation
 *
 * Write back data and metadata for @file to disk.  If @datasync is
 * set only metadata needed to access modified file data is written.
 */
int vfs_fsync(struct file *file, int datasync)
{
	return vfs_fsync_range(file, 0, LLONG_MAX, datasync);
}
EXPORT_SYMBOL(vfs_fsync);

static int do_fsync(unsigned int fd, int datasync)
{
	struct file *file;
	int ret = -EBADF;

	file = fget(fd);
	if (file) {
		ktime_t fsync_t, fsync_diff;
		char pathname[256], *path;
		path = d_path(&(file->f_path), pathname, sizeof(pathname));
		if (IS_ERR(path))
			path = "(unknown)";
		fsync_t = ktime_get();
		ret = vfs_fsync(file, datasync);
		fput(file);
		fsync_diff = ktime_sub(ktime_get(), fsync_t);
		if (ktime_to_ns(fsync_diff) >= 5000000000LL)
			pr_info("VFS: %s pid:%d(%s)(parent:%d/%s) takes %lld nsec to fsync %s.\n", __func__, current->pid, current->comm, current->parent->pid, current->parent->comm, ktime_to_ns(fsync_diff), path);
	}
	return ret;
}

SYSCALL_DEFINE1(fsync, unsigned int, fd)
{
#ifdef CONFIG_DYNAMIC_FSYNC
	if (likely(dyn_fsync_active && !early_suspend_active))
		return 0;
	else
#endif
	return do_fsync(fd, 0);
}

SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
{
#if 0
	if (likely(dyn_fsync_active && !early_suspend_active))
		return 0;
	else
#endif
	return do_fsync(fd, 1);
}

int generic_write_sync(struct file *file, loff_t pos, loff_t count)
{
	if (!(file->f_flags & O_DSYNC) && !IS_SYNC(file->f_mapping->host))
		return 0;
	return vfs_fsync_range(file, pos, pos + count - 1,
			       (file->f_flags & __O_SYNC) ? 0 : 1);
}
EXPORT_SYMBOL(generic_write_sync);

SYSCALL_DEFINE(sync_file_range)(int fd, loff_t offset, loff_t nbytes,
				unsigned int flags)
{
#ifdef CONFIG_DYNAMIC_FSYNC
	if (likely(dyn_fsync_active && !early_suspend_active))
		return 0;
	else {
#endif

	int ret;
	struct file *file;
	struct address_space *mapping;
	loff_t endbyte;			
	int fput_needed;
	umode_t i_mode;

	ret = -EINVAL;
	if (flags & ~VALID_FLAGS)
		goto out;

	endbyte = offset + nbytes;

	if ((s64)offset < 0)
		goto out;
	if ((s64)endbyte < 0)
		goto out;
	if (endbyte < offset)
		goto out;

	if (sizeof(pgoff_t) == 4) {
		if (offset >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			ret = 0;
			goto out;
		}
		if (endbyte >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			nbytes = 0;
		}
	}

	if (nbytes == 0)
		endbyte = LLONG_MAX;
	else
		endbyte--;		

	ret = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto out;

	i_mode = file->f_path.dentry->d_inode->i_mode;
	ret = -ESPIPE;
	if (!S_ISREG(i_mode) && !S_ISBLK(i_mode) && !S_ISDIR(i_mode) &&
			!S_ISLNK(i_mode))
		goto out_put;

	mapping = file->f_mapping;
	if (!mapping) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = 0;
	if (flags & SYNC_FILE_RANGE_WAIT_BEFORE) {
		ret = filemap_fdatawait_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WRITE) {
		ret = filemap_fdatawrite_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WAIT_AFTER)
		ret = filemap_fdatawait_range(mapping, offset, endbyte);

out_put:
	fput_light(file, fput_needed);
out:
	return ret;
#ifdef CONFIG_DYNAMIC_FSYNC
	}
#endif
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_sync_file_range(long fd, loff_t offset, loff_t nbytes,
				    long flags)
{
	return SYSC_sync_file_range((int) fd, offset, nbytes,
				    (unsigned int) flags);
}
SYSCALL_ALIAS(sys_sync_file_range, SyS_sync_file_range);
#endif

SYSCALL_DEFINE(sync_file_range2)(int fd, unsigned int flags,
				 loff_t offset, loff_t nbytes)
{
#ifdef CONFIG_DYNAMIC_FSYNC
	if (likely(dyn_fsync_active && !early_suspend_active))
		return 0;
	else
#endif
	return sys_sync_file_range(fd, offset, nbytes, flags);
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_sync_file_range2(long fd, long flags,
				     loff_t offset, loff_t nbytes)
{
	return SYSC_sync_file_range2((int) fd, (unsigned int) flags,
				     offset, nbytes);
}
SYSCALL_ALIAS(sys_sync_file_range2, SyS_sync_file_range2);
#endif
