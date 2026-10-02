// SPDX-License-Identifier: GPL-2.0-only
/*
 * kext_dynamic_readahead: per-uid control of fault-around and readahead
 * scale, reduced port of the Xiaomi OS4 dynamic_readahead module
 * (ra_order bypass deliberately not ported; see plan T-007).
 *
 * Consumes existing hooks only:
 *  - android_vh_should_fault_around (mm/memory.c): uids on the blacklist
 *    get fault_around disabled (their access pattern thrashes it);
 *  - android_vh_ra_tuning_max_page (mm/readahead.c): uids on the scale
 *    list get their max_pages scaled by the configured percentage.
 *
 * Interfaces: module params enable (default off), fault_around_uid (one
 * uid; 0 clears), ra_scale_uid + ra_scale_pct. One uid per knob keeps
 * the hot paths branch-simple; extend to lists only if ever needed.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <trace/hooks/mm.h>

static bool kext_ra_enable;
static int kext_ra_fault_around_uid;
static int kext_ra_scale_uid = -1;
static unsigned int kext_ra_scale_pct = 100;

static bool kext_ra_match(struct task_struct *t, int uid)
{
	kuid_t u;
	unsigned int val;

	if (!t || uid <= 0)
		return false;
	u = task_uid(t);
	val = from_kuid_munged(current_user_ns(), u);
	return (int)val == uid;
}

static void kext_ra_fault_around(void *data, struct vm_fault *vmf,
				bool *should_around)
{
	if (!kext_ra_enable)
		return;
	if (kext_ra_match(current, kext_ra_fault_around_uid))
		*should_around = false;
}

static void kext_ra_max_page(void *data, struct readahead_control *ractl,
			    unsigned long *max_pages)
{
	unsigned long scaled;

	if (!kext_ra_enable || !max_pages)
		return;
	if (!kext_ra_match(current, kext_ra_scale_uid))
		return;
	if (kext_ra_scale_pct == 100 || !*max_pages)
		return;

	scaled = mult_frac(*max_pages, kext_ra_scale_pct, 100);
	if (scaled < 1)
		scaled = 1;
	*max_pages = scaled;
}

static int __init kext_dynamic_readahead_init(void)
{
	int ret;

	ret = register_trace_android_vh_should_fault_around(
		kext_ra_fault_around, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_ra_tuning_max_page(
		kext_ra_max_page, NULL);
	if (ret) {
		unregister_trace_android_vh_should_fault_around(
			kext_ra_fault_around, NULL);
		return ret;
	}
	return 0;
}

static void __exit kext_dynamic_readahead_exit(void)
{
	unregister_trace_android_vh_should_fault_around(
		kext_ra_fault_around, NULL);
	unregister_trace_android_vh_ra_tuning_max_page(
		kext_ra_max_page, NULL);
	tracepoint_synchronize_unregister();
}

module_init(kext_dynamic_readahead_init);
module_exit(kext_dynamic_readahead_exit);

module_param_named(enable, kext_ra_enable, bool, 0644);
MODULE_PARM_DESC(enable, "enable dynamic readahead overrides (default false)");
module_param_named(fault_around_uid, kext_ra_fault_around_uid, int, 0644);
MODULE_PARM_DESC(fault_around_uid, "uid whose fault_around is disabled (0 = none)");
module_param_named(ra_scale_uid, kext_ra_scale_uid, int, 0644);
MODULE_PARM_DESC(ra_scale_uid, "uid whose readahead window is scaled (-1 = none)");
module_param_named(ra_scale_pct, kext_ra_scale_pct, uint, 0644);
MODULE_PARM_DESC(ra_scale_pct, "readahead window scale percent for ra_scale_uid (1..200)");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kext dynamic readahead: per-uid fault_around/readahead control (reduced port)");
