// SPDX-License-Identifier: GPL-2.0-only
/*
 * kext_unfairmem: relax the allocation watermark for flagged tasks so the
 * renderer / scene threads can dip into the reserve instead of stalling
 * in reclaim while a frame deadline is pending.
 *
 * Hook: android_vh_get_page_wmark (fires right after the fast-path mark
 * is computed for the current zone). The callback LOWERS the mark for
 * sf_pid / scene_tid by a bounded amount (at most half) so those
 * callers may allocate from the reserve instead of stalling.
 *
 * The slab side (vh_shrink_slab_bypass) is NOT registered here: mm/slabd.c
 * consumes it already, and the VIP check is merged into its callback via
 * kext_unfairmem_is_vip() below (single-consumer discipline; avoids the
 * double-registration async residue).
 *
 * Port of the Xiaomi OS4 unfairmem module idea ("adjust min watermark
 * for important task"), rewritten for 5.15 with a hard cap: the mark is
 * raised by at most half of the scale-factor-derived gap, so a runaway
 * configuration cannot hand the reserve to the renderer wholesale.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <trace/hooks/mm.h>

static pid_t kext_unfair_sf_pid;
static pid_t kext_unfair_scene_tid;

/* Bounded relax: at most 50% below the computed mark. */
static unsigned int kext_unfair_relax_pct = 50;

/*
 * VIP check for mm/slabd.c, registered through the kext_slabd_vip_check
 * function pointer (slabd is built-in; direct symbol reference would
 * not link when this is a module).
 */
static bool kext_unfairmem_is_vip(struct task_struct *t)
{
	if (!t)
		return false;
	if (!kext_unfair_sf_pid && !kext_unfair_scene_tid)
		return false;
	return task_tgid_nr(t) == kext_unfair_sf_pid ||
	       t->pid == kext_unfair_scene_tid;
}

static void kext_unfair_wmark(void *data, unsigned int alloc_flags,
			     unsigned long *page_wmark)
{
	unsigned long mark, relaxed;

	if (!page_wmark || !kext_unfairmem_is_vip(current))
		return;

	/*
	 * Lower the bar, not raise it: __zone_watermark_ok() passes when
	 * free pages exceed the mark, so a *smaller* mark lets the flagged
	 * caller dip into the reserve instead of stalling. Raising it
	 * would push them into the slowpath sooner (reviewer finding).
	 * Bounded: never below half of the computed mark.
	 */
	mark = *page_wmark;
	relaxed = mark - mult_frac(mark, kext_unfair_relax_pct, 100);
	if (relaxed < mark / 2)
		relaxed = mark / 2;
	*page_wmark = relaxed;
}

static int kext_unfair_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "sf_pid=%d\nscene_tid=%d\nrelax_pct=%u\n",
		   kext_unfair_sf_pid, kext_unfair_scene_tid,
		   kext_unfair_relax_pct);
	return 0;
}

static int kext_unfair_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, kext_unfair_proc_show, NULL);
}

static ssize_t kext_unfair_proc_write(struct file *file,
				     const char __user *buf, size_t count,
				     loff_t *ppos)
{
	char tmp[64];
	long val;
	int field;

	if (count >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, count))
		return -EFAULT;
	tmp[count] = '\0';

	if (sscanf(tmp, "sf_pid %ld", &val) == 1)
		field = 0;
	else if (sscanf(tmp, "scene_tid %ld", &val) == 1)
		field = 1;
	else if (sscanf(tmp, "relax_pct %ld", &val) == 1)
		field = 2;
	else
		return -EINVAL;

	switch (field) {
	/*
	 * pid_max is not exported to modules on 5.15; pid_t is a signed
	 * int and the hook side re-checks against real tasks, so a plain
	 * non-negative bound is enough here.
	 */
	case 0:
		if (val < 0)
			return -EINVAL;
		WRITE_ONCE(kext_unfair_sf_pid, (pid_t)val);
		break;
	case 1:
		if (val < 0)
			return -EINVAL;
		WRITE_ONCE(kext_unfair_scene_tid, (pid_t)val);
		break;
	case 2:
		if (val < 0 || val > 50)
			return -EINVAL;
		WRITE_ONCE(kext_unfair_relax_pct, (unsigned int)val);
		break;
	}
	return count;
}

static const struct proc_ops kext_unfair_proc_ops = {
	.proc_open	= kext_unfair_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= kext_unfair_proc_write,
};

extern bool (*kext_slabd_vip_check)(struct task_struct *t);

static int __init kext_unfairmem_init(void)
{
	int ret;

	if (!proc_create("kext_unfairmem", 0644, NULL, &kext_unfair_proc_ops))
		return -ENOMEM;
	ret = register_trace_android_vh_get_page_wmark(kext_unfair_wmark, NULL);
	if (ret) {
		remove_proc_entry("kext_unfairmem", NULL);
		return ret;
	}
	WRITE_ONCE(kext_slabd_vip_check, kext_unfairmem_is_vip);
	return 0;
}

static void __exit kext_unfairmem_exit(void)
{
	WRITE_ONCE(kext_slabd_vip_check, NULL);
	unregister_trace_android_vh_get_page_wmark(kext_unfair_wmark, NULL);
	tracepoint_synchronize_unregister();
	remove_proc_entry("kext_unfairmem", NULL);
}

module_init(kext_unfairmem_init);
module_exit(kext_unfairmem_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kext unfairmem: bounded watermark relax for flagged (sf/scene) tasks");
