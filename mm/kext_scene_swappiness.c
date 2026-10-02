// SPDX-License-Identifier: GPL-2.0-only
/*
 * kext_scene_swappiness: select the reclaim swappiness value by scene.
 *
 * Hooks android_vh_tune_swappiness, which fires at both swappiness
 * decision points in mm/vmscan.c (classic-LRU get_scan_count() and the
 * MGLRU path). Both call sites pass the value they already computed;
 * a callback may rewrite it in place or leave it untouched.
 *
 * The scene is selected by writing its name to
 * /proc/kext_scene_swappiness/scene ("default"/"game"/"camera"/"browser");
 * hook counters are in /proc/kext_scene_swappiness/stats.
 *
 * PASSTHROUGH CONTRACT: scene "default" (the boot state) must never
 * write *swappiness. The additional modules set vm.swappiness=1 as the
 * standing policy; that stays authoritative only if we leave the hook
 * value untouched unless a non-default scene is explicitly armed. The
 * early return in the callback below is the coexistence lifeline.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <trace/hooks/vmscan.h>

/*
 * Scene -> swappiness table. -1 = do not intervene (passthrough).
 * This is the tuning surface: adjust values and rebuild. game=0 keeps
 * anon pages out of swap for the foreground game; camera=10 and
 * browser=20 allow mild anon reclaim to absorb allocation bursts.
 */
struct kext_scene_entry {
	const char *name;
	int swappiness;
};

static const struct kext_scene_entry kext_scenes[] = {
	{ "default",	-1 },
	{ "game",	 0 },
	{ "camera",	10 },
	{ "browser",	20 },
};

/* atomic index into kext_scenes; 0 == "default" == passthrough */
static atomic_t kext_scene_idx = ATOMIC_INIT(0);
static atomic64_t kext_stat_reads;
static atomic64_t kext_stat_tunes;

static void kext_tune_swappiness(void *data, int *swappiness)
{
	int idx;

	atomic64_inc(&kext_stat_reads);

	/* PASSTHROUGH CONTRACT (see file comment): no write to
	 * *swappiness unless a non-default scene is armed. An out-of-range
	 * index also falls through as passthrough.
	 */
	idx = atomic_read(&kext_scene_idx);
	if (idx <= 0 || idx >= (int)ARRAY_SIZE(kext_scenes))
		return;

	*swappiness = kext_scenes[idx].swappiness;
	atomic64_inc(&kext_stat_tunes);
}

static int kext_scene_show(struct seq_file *m, void *v)
{
	int idx = atomic_read(&kext_scene_idx);

	if (idx < 0 || idx >= (int)ARRAY_SIZE(kext_scenes))
		idx = 0;
	seq_printf(m, "%s %d\n", kext_scenes[idx].name,
		   kext_scenes[idx].swappiness);
	return 0;
}

static int kext_scene_open(struct inode *inode, struct file *file)
{
	return single_open(file, kext_scene_show, NULL);
}

static ssize_t kext_scene_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char kbuf[16];
	int i, idx = -1;

	if (count == 0 || count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	strim(kbuf);

	for (i = 0; i < (int)ARRAY_SIZE(kext_scenes); i++) {
		if (!strcmp(kbuf, kext_scenes[i].name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return -EINVAL;

	atomic_set(&kext_scene_idx, idx);
	return count;
}

static const struct proc_ops kext_scene_proc_ops = {
	.proc_open	= kext_scene_open,
	.proc_read	= seq_read,
	.proc_write	= kext_scene_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int kext_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "reads: %lld\n",
		   (long long)atomic64_read(&kext_stat_reads));
	seq_printf(m, "tunes: %lld\n",
		   (long long)atomic64_read(&kext_stat_tunes));
	return 0;
}

static int kext_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, kext_stats_show, NULL);
}

static const struct proc_ops kext_stats_proc_ops = {
	.proc_open	= kext_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static struct proc_dir_entry *kext_proc_dir;

static int __init kext_scene_swappiness_init(void)
{
	int ret;

	ret = -ENOMEM;
	kext_proc_dir = proc_mkdir("kext_scene_swappiness", NULL);
	if (!kext_proc_dir)
		return ret;

	if (!proc_create("scene", 0644, kext_proc_dir, &kext_scene_proc_ops) ||
	    !proc_create("stats", 0444, kext_proc_dir, &kext_stats_proc_ops))
		goto err_proc;

	ret = register_trace_android_vh_tune_swappiness(kext_tune_swappiness,
							NULL);
	if (ret)
		goto err_proc;

	pr_info("kext_scene_swappiness: registered (scene=default, passthrough)\n");
	return 0;

err_proc:
	remove_proc_subtree("kext_scene_swappiness", NULL);
	return ret;
}

static void __exit kext_scene_swappiness_exit(void)
{
	unregister_trace_android_vh_tune_swappiness(kext_tune_swappiness,
						    NULL);
	/* guarantee no CPU is still inside our callback before the code
	 * goes away */
	tracepoint_synchronize_unregister();
	remove_proc_subtree("kext_scene_swappiness", NULL);
	pr_info("kext_scene_swappiness: unregistered\n");
}

module_init(kext_scene_swappiness_init);
module_exit(kext_scene_swappiness_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kext scene-based reclaim swappiness tuning");
MODULE_AUTHOR("Kext");
