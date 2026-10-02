// SPDX-License-Identifier: GPL-2.0-only
/*
 * kext_alloc_adjust: drop __GFP_RECLAIM for high-order DMA-IOMMU allocations
 * to reduce reclaim-induced jank under memory pressure.
 *
 * Faithful port of Oplus kswapd_opt's alloc_adjust_flags + kvmalloc_adjust_flags
 * callbacks, wired to the already-present vendor hooks:
 *   - android_vh_adjust_alloc_flags        (order > PAGE_ALLOC_COSTLY_ORDER)
 *   - android_vh_kvmalloc_node_use_vmalloc (get_order(size) > costly order)
 *
 * For large (>= 32KB / order > 3) allocations, clearing __GFP_RECLAIM prevents
 * the allocator from kicking off kswapd/direct-reclaim to scrape together
 * contiguous pages. Such allocations either succeed from the free lists or
 * fall back to the next order, instead of blocking the caller in reclaim.
 * This is the "high-order DMA-IOMMU alloc flags" optimization for smoothness.
 *
 * Runtime control: /sys/kernel/kext_alloc_adjust/enabled (0/1).
 * The hook body is a single static-key branch; when disabled the hot path is
 * a no-op jump.
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/jump_label.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <trace/hooks/iommu.h>
#include <trace/hooks/mm.h>

static DEFINE_STATIC_KEY_FALSE(kext_alloc_adjust_key);

/* Runtime switch state (mirrors the static key). */
static bool kext_alloc_adjust_enabled = true;

static struct kobject *kext_alloc_adjust_kobj;

static void kext_alloc_adjust_set(bool on)
{
	if (on)
		static_branch_enable(&kext_alloc_adjust_key);
	else
		static_branch_disable(&kext_alloc_adjust_key);
	kext_alloc_adjust_enabled = on;
}

/* iommu: high-order DMA-IOMMU allocation flags */
static void kext_alloc_adjust_flags(void *data, unsigned int order, gfp_t *flags)
{
	if (!static_branch_likely(&kext_alloc_adjust_key))
		return;

	if (order > PAGE_ALLOC_COSTLY_ORDER)
		*flags &= ~__GFP_RECLAIM;
}

/* mm: kvmalloc that decides between kmalloc and vmalloc */
static void kext_kvmalloc_adjust_flags(void *data, size_t size,
				      gfp_t *kvmalloc_flags, bool *unused)
{
	if (!static_branch_likely(&kext_alloc_adjust_key))
		return;

	if (get_order(size) > PAGE_ALLOC_COSTLY_ORDER)
		*kvmalloc_flags &= ~__GFP_RECLAIM;
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%d\n", kext_alloc_adjust_enabled ? 1 : 0);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	kext_alloc_adjust_set(on);

	return len;
}

static struct kobj_attribute enabled_attr = __ATTR(enabled, 0644,
						   enabled_show, enabled_store);

static struct attribute *kext_alloc_adjust_attrs[] = {
	&enabled_attr.attr,
	NULL,
};

static struct attribute_group kext_alloc_adjust_attr_group = {
	.name = NULL, /* attach directly under /sys/kernel/kext_alloc_adjust/ */
	.attrs = kext_alloc_adjust_attrs,
};

static int __init kext_alloc_adjust_init(void)
{
	int ret;

	kext_alloc_adjust_set(true);

	ret = register_trace_android_vh_adjust_alloc_flags(
		kext_alloc_adjust_flags, NULL);
	if (ret)
		goto out_no_hooks;

	ret = register_trace_android_vh_kvmalloc_node_use_vmalloc(
		kext_kvmalloc_adjust_flags, NULL);
	if (ret)
		goto out_unreg_iommu;

	kext_alloc_adjust_kobj = kobject_create_and_add("kext_alloc_adjust",
						       kernel_kobj);
	if (kext_alloc_adjust_kobj) {
		ret = sysfs_create_group(kext_alloc_adjust_kobj,
					 &kext_alloc_adjust_attr_group);
		if (ret)
			pr_warn("kext_alloc_adjust: sysfs group failed %d\n",
				ret);
	}

	pr_info("kext_alloc_adjust: registered (enabled=%d)\n",
		kext_alloc_adjust_enabled ? 1 : 0);
	return 0;

out_unreg_iommu:
	unregister_trace_android_vh_adjust_alloc_flags(
		kext_alloc_adjust_flags, NULL);
out_no_hooks:
	return ret;
}

late_initcall(kext_alloc_adjust_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kext high-order DMA-IOMMU alloc flags adjustment");
MODULE_AUTHOR("Kext");
