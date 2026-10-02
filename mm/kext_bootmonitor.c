// SPDX-License-Identifier: GPL-2.0-only
/*
 * kext_bootmonitor: boot-event logger, reduced port of the Xiaomi OS4
 * bootmonitor module (drivers/xiaomi/bootmonitor in dada-v-oss).
 *
 * What is kept from the original: the ordered boot-event anchor model
 * and the bootmode/fingerprint stamping from module params. The anchor
 * table below is a RENAMED, shorter table inspired by the original
 * (the OS4 table is early-init/late-init/late-fs/zygote-start/.../
 * boot_completed in boot_monitor.c:31-41); names here match what a
 * GKI ramdisk init.rc can realistically report.
 *
 * What is deliberately NOT ported: the DT /reserved-memory resource
 * probe ("xiaomi,bootmonitor_pmsg" node absent on nuwa/vermeer/fuxi)
 * and the raw blackbox-partition block-device writer. Persistence is
 * MUCH weaker than the original blackbox: the summary is emitted via
 * pr_info into the kernel log ring, and console-ramoops (enabled in
 * this kernel) persists only the TAIL of that ring across a reboot.
 * In practice: a boot-loop or a reboot shortly after boot keeps the
 * summary; a long-running session that reboots may have evicted it.
 * A failed boot that never reaches the last anchor also produces no
 * summary at all — the per-anchor hits are still visible in the
 * console log itself.
 *
 * Built as a module the anchor timestamps are relative to modprobe
 * time, not kernel start: use CONFIG_KEXT_BOOTMONITOR=y for real
 * kernel-relative timings.
 *
 * Interfaces: /proc/kext_bootmonitor (read the anchor table, write
 * anchor names strictly in order), module params bootmode/fingerprint.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ktime.h>

#define KEXT_BM_EVENTS_MAX	32
#define KEXT_BM_NAME_LEN		32

struct kext_bm_event {
	char	name[KEXT_BM_NAME_LEN];
	u64	ts_ns;		/* 0 = not yet reached */
	int	status;		/* 0 pending, 1 hit */
};

/*
 * Boot-event anchors (renamed table inspired by the OS4 original;
 * warntime not enforced — this port records, it does not watchdog.
 * Extend as needed.)
 */
static const char * const kext_bm_anchor_names[] = {
	"kernel-init",
	"driver-init-done",
	"init-start",
	"early-fs",
	"fs-mounted",
	"late-fs",
	"boot-complete",
};

#define KEXT_BM_ANCHORS (sizeof(kext_bm_anchor_names) / sizeof(kext_bm_anchor_names[0]))

static struct kext_bm_event kext_bm_events[KEXT_BM_ANCHORS];
static u64 kext_bm_boot_start_ns;
static atomic_t kext_bm_next;

static char kext_bm_bootmode[16] = "unknown";
module_param_string(bootmode, kext_bm_bootmode, sizeof(kext_bm_bootmode), 0644);

static char kext_bm_fingerprint[128] = "unknown";
module_param_string(fingerprint, kext_bm_fingerprint, sizeof(kext_bm_fingerprint), 0644);

/*
 * Mirror the finalized event table into the kernel log ring so it survives
 * the next reboot (console-ramoops tail on this platform). Failure is
 * non-fatal: the in-memory table remains readable via /proc.
 */
static void kext_bm_pstore_flush(void)
{
	char *msg;
	int i, off;

	msg = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	off = scnprintf(msg, PAGE_SIZE,
			"kext-bootmonitor: bootmode=%s fingerprint=%s\n",
			kext_bm_bootmode, kext_bm_fingerprint);
	for (i = 0; i < KEXT_BM_ANCHORS && off < PAGE_SIZE - 64; i++) {
		if (!kext_bm_events[i].status)
			continue;
		off += scnprintf(msg + off, PAGE_SIZE - off,
				 "kext-bootmonitor: [%d] %-20s %10llu ns\n", i,
				 kext_bm_anchor_names[i],
				 kext_bm_events[i].ts_ns);
	}

	/*
	 * console-ramoops persists the kernel log ring across reboots on
	 * this platform; emitting the summary with a stable prefix makes
	 * it retrievable from /sys/fs/pstore/console-ramoops after the
	 * next boot. No pstore_info registration needed (that path is
	 * backend-only).
	 */
	pr_info("kext-bootmonitor: boot summary:\n%s", msg);
	kfree(msg);
}

/*
 * Record the next anchor in order. Writing an arbitrary name out of
 * order is rejected: anchors are sequential by design.
 */
/* Serializes anchor writes; init is the only expected writer, but a
 * stray second writer must not be able to double-advance the index.
 */
static DEFINE_MUTEX(kext_bm_write_lock);

static ssize_t kext_bm_proc_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char tmp[KEXT_BM_NAME_LEN];
	int idx;

	if (count == 0 || count >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, count))
		return -EFAULT;
	tmp[count] = '\0';
	if (tmp[count - 1] == '\n')
		tmp[count - 1] = '\0';

	mutex_lock(&kext_bm_write_lock);
	idx = atomic_read(&kext_bm_next);
	if (idx < 0 || idx >= KEXT_BM_ANCHORS) {
		mutex_unlock(&kext_bm_write_lock);
		return -ENOSPC;
	}
	if (strcmp(tmp, kext_bm_anchor_names[idx]) != 0) {
		mutex_unlock(&kext_bm_write_lock);
		return -EINVAL; /* out-of-order or unknown anchor */
	}

	kext_bm_events[idx].ts_ns = ktime_get_boottime_ns() - kext_bm_boot_start_ns;
	kext_bm_events[idx].status = 1;
	atomic_inc(&kext_bm_next);
	mutex_unlock(&kext_bm_write_lock);

	if (idx == KEXT_BM_ANCHORS - 1)
		kext_bm_pstore_flush();

	return count;
}

static int kext_bm_proc_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "bootmode=%s fingerprint=%s\n", kext_bm_bootmode,
		   kext_bm_fingerprint);
	for (i = 0; i < KEXT_BM_ANCHORS; i++) {
		seq_printf(m, "[%d] %-20s %-10s %10llu ns\n", i,
			   kext_bm_anchor_names[i],
			   kext_bm_events[i].status ? "hit" : "pending",
			   kext_bm_events[i].ts_ns);
	}
	return 0;
}

static int kext_bm_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, kext_bm_proc_show, NULL);
}

static const struct proc_ops kext_bm_proc_ops = {
	.proc_open	= kext_bm_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= kext_bm_proc_write,
};

static int __init kext_bootmonitor_init(void)
{
	kext_bm_boot_start_ns = ktime_get_boottime_ns();
	if (!proc_create("kext_bootmonitor", 0644, NULL, &kext_bm_proc_ops))
		return -ENOMEM;
	return 0;
}

static void __exit kext_bootmonitor_exit(void)
{
	/*
	 * Partial-table bailout: if the module is unloaded (manual or
	 * debug rmmod) before the last anchor was hit, emit whatever was
	 * recorded so the console-ramoops tail at least carries it.
	 */
	if (atomic_read(&kext_bm_next) < KEXT_BM_ANCHORS) {
		mutex_lock(&kext_bm_write_lock);
		kext_bm_pstore_flush();
		mutex_unlock(&kext_bm_write_lock);
	}
	remove_proc_entry("kext_bootmonitor", NULL);
}

module_init(kext_bootmonitor_init);
module_exit(kext_bootmonitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kext boot-event logger (OS4 bootmonitor reduced port, console-ramoops-tailed)");
