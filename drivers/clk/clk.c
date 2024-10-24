// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Linaro Ltd <mturquette@linaro.org>
 *
 * Standard functionality for the common clock API.  See Documentation/driver-api/clk.rst
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/clk-conf.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/clkdev.h>

#include "clk.h"

static DEFINE_SPINLOCK(enable_lock);
static DEFINE_MUTEX(prepare_lock);

static struct task_struct *prepare_owner;
static struct task_struct *enable_owner;

static int prepare_refcnt;
static int enable_refcnt;

static HLIST_HEAD(clk_root_list);
static HLIST_HEAD(clk_orphan_list);
static LIST_HEAD(clk_notifier_list);

static struct hlist_head *all_lists[] = {
	&clk_root_list,
	&clk_orphan_list,
	NULL,
};

/***    private data structures    ***/

struct clk_parent_map {
	const struct clk_hw	*hw;
	struct clk_core		*core;
	const char		*fw_name;
	const char		*name;
	int			index;
};

struct clk_core {
	const char		*name;
	const struct clk_ops	*ops;
	struct clk_hw		*hw;
	struct module		*owner;
	struct device		*dev;
	struct device_node	*of_node;
	struct clk_core		*parent;
	struct clk_parent_map	*parents;
	u8			num_parents;
	u8			new_parent_index;
	unsigned long		rate;
	unsigned long		req_rate;
/*
 * IAMROOT, 2022.08.13:
 * - pre로 clock을 계산해볼때 결과값을 저장하는 멤버.
 */
	unsigned long		new_rate;
	struct clk_core		*new_parent;
	struct clk_core		*new_child;
	unsigned long		flags;
	bool			orphan;
	bool			rpm_enabled;
	unsigned int		enable_count;
	unsigned int		prepare_count;
	unsigned int		protect_count;
	unsigned long		min_rate;
	unsigned long		max_rate;
	unsigned long		accuracy;
	int			phase;
	struct clk_duty		duty;
	struct hlist_head	children;
	struct hlist_node	child_node;
	struct hlist_head	clks;
	unsigned int		notifier_count;
#ifdef CONFIG_DEBUG_FS
	struct dentry		*dentry;
	struct hlist_node	debug_node;
#endif
	struct kref		ref;
};

#define CREATE_TRACE_POINTS
#include <trace/events/clk.h>

struct clk {
	struct clk_core	*core;
	struct device *dev;
	const char *dev_id;
	const char *con_id;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned int exclusive_count;
	struct hlist_node clks_node;
};

/***           runtime pm          ***/
/*
 * IAMROOT, 2022.08.13:
 * - 절전 상태에 있으면 normal 상태로 전환.
 */
static int clk_pm_runtime_get(struct clk_core *core)
{
	int ret;

	if (!core->rpm_enabled)
		return 0;

	ret = pm_runtime_get_sync(core->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(core->dev);
		return ret;
	}
	return 0;
}

static void clk_pm_runtime_put(struct clk_core *core)
{
	if (!core->rpm_enabled)
		return;

	pm_runtime_put_sync(core->dev);
}

/***           locking             ***/
static void clk_prepare_lock(void)
{
	if (!mutex_trylock(&prepare_lock)) {
		if (prepare_owner == current) {
			prepare_refcnt++;
			return;
		}
		mutex_lock(&prepare_lock);
	}
	WARN_ON_ONCE(prepare_owner != NULL);
	WARN_ON_ONCE(prepare_refcnt != 0);
	prepare_owner = current;
	prepare_refcnt = 1;
}

static void clk_prepare_unlock(void)
{
	WARN_ON_ONCE(prepare_owner != current);
	WARN_ON_ONCE(prepare_refcnt == 0);

	if (--prepare_refcnt)
		return;
	prepare_owner = NULL;
	mutex_unlock(&prepare_lock);
}

/*
 * IAMROOT, 2022.08.13:
 * - 전역 lock enable
 */
static unsigned long clk_enable_lock(void)
	__acquires(enable_lock)
{
	unsigned long flags;

	/*
	 * On UP systems, spin_trylock_irqsave() always returns true, even if
	 * we already hold the lock. So, in that case, we rely only on
	 * reference counting.
	 */
	if (!IS_ENABLED(CONFIG_SMP) ||
	    !spin_trylock_irqsave(&enable_lock, flags)) {
		if (enable_owner == current) {
			enable_refcnt++;
			__acquire(enable_lock);
			if (!IS_ENABLED(CONFIG_SMP))
				local_save_flags(flags);
			return flags;
		}
		spin_lock_irqsave(&enable_lock, flags);
	}
	WARN_ON_ONCE(enable_owner != NULL);
	WARN_ON_ONCE(enable_refcnt != 0);
	enable_owner = current;
	enable_refcnt = 1;
	return flags;
}

static void clk_enable_unlock(unsigned long flags)
	__releases(enable_lock)
{
	WARN_ON_ONCE(enable_owner != current);
	WARN_ON_ONCE(enable_refcnt == 0);

	if (--enable_refcnt) {
		__release(enable_lock);
		return;
	}
	enable_owner = NULL;
	spin_unlock_irqrestore(&enable_lock, flags);
}

/*
 * IAMROOT, 2022.08.13:
 * - 누군가 protected해논지 확인.
 */
static bool clk_core_rate_is_protected(struct clk_core *core)
{
	return core->protect_count;
}

static bool clk_core_is_prepared(struct clk_core *core)
{
	bool ret = false;

	/*
	 * .is_prepared is optional for clocks that can prepare
	 * fall back to software usage counter if it is missing
	 */
	if (!core->ops->is_prepared)
		return core->prepare_count;

	if (!clk_pm_runtime_get(core)) {
		ret = core->ops->is_prepared(core->hw);
		clk_pm_runtime_put(core);
	}

	return ret;
}

static bool clk_core_is_enabled(struct clk_core *core)
{
	bool ret = false;

	/*
	 * .is_enabled is only mandatory for clocks that gate
	 * fall back to software usage counter if .is_enabled is missing
	 */
	if (!core->ops->is_enabled)
		return core->enable_count;

	/*
	 * Check if clock controller's device is runtime active before
	 * calling .is_enabled callback. If not, assume that clock is
	 * disabled, because we might be called from atomic context, from
	 * which pm_runtime_get() is not allowed.
	 * This function is called mainly from clk_disable_unused_subtree,
	 * which ensures proper runtime pm activation of controller before
	 * taking enable spinlock, but the below check is needed if one tries
	 * to call it from other places.
	 */
	if (core->rpm_enabled) {
		pm_runtime_get_noresume(core->dev);
		if (!pm_runtime_active(core->dev)) {
			ret = false;
			goto done;
		}
	}

	ret = core->ops->is_enabled(core->hw);
done:
	if (core->rpm_enabled)
		pm_runtime_put(core->dev);

	return ret;
}

/***    helper functions   ***/

const char *__clk_get_name(const struct clk *clk)
{
	return !clk ? NULL : clk->core->name;
}
EXPORT_SYMBOL_GPL(__clk_get_name);

const char *clk_hw_get_name(const struct clk_hw *hw)
{
	return hw->core->name;
}
EXPORT_SYMBOL_GPL(clk_hw_get_name);

struct clk_hw *__clk_get_hw(struct clk *clk)
{
	return !clk ? NULL : clk->core->hw;
}
EXPORT_SYMBOL_GPL(__clk_get_hw);

unsigned int clk_hw_get_num_parents(const struct clk_hw *hw)
{
	return hw->core->num_parents;
}
EXPORT_SYMBOL_GPL(clk_hw_get_num_parents);

struct clk_hw *clk_hw_get_parent(const struct clk_hw *hw)
{
	return hw->core->parent ? hw->core->parent->hw : NULL;
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent);

static struct clk_core *__clk_lookup_subtree(const char *name,
					     struct clk_core *core)
{
	struct clk_core *child;
	struct clk_core *ret;

	if (!strcmp(core->name, name))
		return core;

	hlist_for_each_entry(child, &core->children, child_node) {
		ret = __clk_lookup_subtree(name, child);
		if (ret)
			return ret;
	}

	return NULL;
}

static struct clk_core *clk_core_lookup(const char *name)
{
	struct clk_core *root_clk;
	struct clk_core *ret;

	if (!name)
		return NULL;

	/* search the 'proper' clk tree first */
	hlist_for_each_entry(root_clk, &clk_root_list, child_node) {
		ret = __clk_lookup_subtree(name, root_clk);
		if (ret)
			return ret;
	}

	/* if not found, then search the orphan tree */
	hlist_for_each_entry(root_clk, &clk_orphan_list, child_node) {
		ret = __clk_lookup_subtree(name, root_clk);
		if (ret)
			return ret;
	}

	return NULL;
}

#ifdef CONFIG_OF
static int of_parse_clkspec(const struct device_node *np, int index,
			    const char *name, struct of_phandle_args *out_args);
static struct clk_hw *
of_clk_get_hw_from_clkspec(struct of_phandle_args *clkspec);
#else
static inline int of_parse_clkspec(const struct device_node *np, int index,
				   const char *name,
				   struct of_phandle_args *out_args)
{
	return -ENOENT;
}
static inline struct clk_hw *
of_clk_get_hw_from_clkspec(struct of_phandle_args *clkspec)
{
	return ERR_PTR(-ENOENT);
}
#endif

/**
 * clk_core_get - Find the clk_core parent of a clk
 * @core: clk to find parent of
 * @p_index: parent index to search for
 *
 * This is the preferred method for clk providers to find the parent of a
 * clk when that parent is external to the clk controller. The parent_names
 * array is indexed and treated as a local name matching a string in the device
 * node's 'clock-names' property or as the 'con_id' matching the device's
 * dev_name() in a clk_lookup. This allows clk providers to use their own
 * namespace instead of looking for a globally unique parent string.
 *
 * For example the following DT snippet would allow a clock registered by the
 * clock-controller@c001 that has a clk_init_data::parent_data array
 * with 'xtal' in the 'name' member to find the clock provided by the
 * clock-controller@f00abcd without needing to get the globally unique name of
 * the xtal clk.
 *
 *      parent: clock-controller@f00abcd {
 *              reg = <0xf00abcd 0xabcd>;
 *              #clock-cells = <0>;
 *      };
 *
 *      clock-controller@c001 {
 *              reg = <0xc001 0xf00d>;
 *              clocks = <&parent>;
 *              clock-names = "xtal";
 *              #clock-cells = <1>;
 *      };
 *
 * Returns: -ENOENT when the provider can't be found or the clk doesn't
 * exist in the provider or the name can't be found in the DT node or
 * in a clkdev lookup. NULL when the provider knows about the clk but it
 * isn't provided on this system.
 * A valid clk_core pointer when the clk can be found in the provider.
 */
static struct clk_core *clk_core_get(struct clk_core *core, u8 p_index)
{
	const char *name = core->parents[p_index].fw_name;
	int index = core->parents[p_index].index;
	struct clk_hw *hw = ERR_PTR(-ENOENT);
	struct device *dev = core->dev;
	const char *dev_id = dev ? dev_name(dev) : NULL;
	struct device_node *np = core->of_node;
	struct of_phandle_args clkspec;

	if (np && (name || index >= 0) &&
	    !of_parse_clkspec(np, index, name, &clkspec)) {
		hw = of_clk_get_hw_from_clkspec(&clkspec);
		of_node_put(clkspec.np);
	} else if (name) {
		/*
		 * If the DT search above couldn't find the provider fallback to
		 * looking up via clkdev based clk_lookups.
		 */
		hw = clk_find_hw(dev_id, name);
	}

	if (IS_ERR(hw))
		return ERR_CAST(hw);

	return hw->core;
}

static void clk_core_fill_parent_index(struct clk_core *core, u8 index)
{
	struct clk_parent_map *entry = &core->parents[index];
	struct clk_core *parent;

	if (entry->hw) {
		parent = entry->hw->core;
		/*
		 * We have a direct reference but it isn't registered yet?
		 * Orphan it and let clk_reparent() update the orphan status
		 * when the parent is registered.
		 */
		if (!parent)
			parent = ERR_PTR(-EPROBE_DEFER);
	} else {
		parent = clk_core_get(core, index);
		if (PTR_ERR(parent) == -ENOENT && entry->name)
			parent = clk_core_lookup(entry->name);
	}

	/* Only cache it if it's not an error */
	if (!IS_ERR(parent))
		entry->core = parent;
}

/*
 * IAMROOT, 2022.08.06:
 * - parent가 없으면 NULL. 
 */
static struct clk_core *clk_core_get_parent_by_index(struct clk_core *core,
							 u8 index)
{
	if (!core || index >= core->num_parents || !core->parents)
		return NULL;

	if (!core->parents[index].core)
		clk_core_fill_parent_index(core, index);

	return core->parents[index].core;
}

struct clk_hw *
clk_hw_get_parent_by_index(const struct clk_hw *hw, unsigned int index)
{
	struct clk_core *parent;

	parent = clk_core_get_parent_by_index(hw->core, index);

	return !parent ? NULL : parent->hw;
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent_by_index);

unsigned int __clk_get_enable_count(struct clk *clk)
{
	return !clk ? 0 : clk->core->enable_count;
}

/*
 * IAMROOT, 2022.08.13:
 * - @core의 rate를 가져온다.
 */
static unsigned long clk_core_get_rate_nolock(struct clk_core *core)
{
	if (!core)
		return 0;

	if (!core->num_parents || core->parent)
		return core->rate;

	/*
	 * Clk must have a parent because num_parents > 0 but the parent isn't
	 * known yet. Best to return 0 as the rate of this clk until we can
	 * properly recalc the rate based on the parent's rate.
	 */
/*
 * IAMROOT, 2022.08.13:
 * - @core parent가 있지만 아지만 parent가 설정이 안된 경우 0으로 그냥 return한다는것.
 */
	return 0;
}

unsigned long clk_hw_get_rate(const struct clk_hw *hw)
{
	return clk_core_get_rate_nolock(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_get_rate);

static unsigned long clk_core_get_accuracy_no_lock(struct clk_core *core)
{
	if (!core)
		return 0;

	return core->accuracy;
}

unsigned long clk_hw_get_flags(const struct clk_hw *hw)
{
	return hw->core->flags;
}
EXPORT_SYMBOL_GPL(clk_hw_get_flags);

bool clk_hw_is_prepared(const struct clk_hw *hw)
{
	return clk_core_is_prepared(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_is_prepared);

bool clk_hw_rate_is_protected(const struct clk_hw *hw)
{
	return clk_core_rate_is_protected(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_rate_is_protected);

bool clk_hw_is_enabled(const struct clk_hw *hw)
{
	return clk_core_is_enabled(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_is_enabled);

bool __clk_is_enabled(struct clk *clk)
{
	if (!clk)
		return false;

	return clk_core_is_enabled(clk->core);
}
EXPORT_SYMBOL_GPL(__clk_is_enabled);

/*
 * IAMROOT, 2022.08.13:
 * - closest일 경우 절대값으로 차이가 적은 것을,(요구 rate보다 높아도 무관)
 *   아니면 best < now <= rate(요구 rate보다 높으면 안됨)
 *   결국 요구 rate보다 높아도되는지 안되는지에대한 차이.
 */
static bool mux_is_better_rate(unsigned long rate, unsigned long now,
			   unsigned long best, unsigned long flags)
{
	if (flags & CLK_MUX_ROUND_CLOSEST)
		return abs(now - rate) < abs(best - rate);

	return now <= rate && now > best;
}

/*
 * IAMROOT, 2022.08.13:
 * - parent를 순회해 best rate를 구해온다.
 */
int clk_mux_determine_rate_flags(struct clk_hw *hw,
				 struct clk_rate_request *req,
				 unsigned long flags)
{
	struct clk_core *core = hw->core, *parent, *best_parent = NULL;
	int i, num_parents, ret;
	unsigned long best = 0;
	struct clk_rate_request parent_req = *req;

	/* if NO_REPARENT flag set, pass through to current parent */
/*
 * IAMROOT, 2022.08.13:
 * - 현재 clk에 parent(입력소스) 변경 불가인 경우.
 *   parent rate 변경이 가능 -> parent rate 변경 시도.
 *	parent 가 있음          -> parent rate 가져옴.
 *	parent 가 없음          -> 자신의 rate 가져옴.
 *	즉 parent rate변경이 불가능한 거면 parent나 자신것을 그대로 쓴다는 의미.
 */
	if (core->flags & CLK_SET_RATE_NO_REPARENT) {
		parent = core->parent;
/*
 * IAMROOT, 2022.08.13:
 * - 자신의 clk source 변경은 없고, 최적의 parent의 rate를 선택하게 한다.
 */
		if (core->flags & CLK_SET_RATE_PARENT) {
			ret = __clk_determine_rate(parent ? parent->hw : NULL,
						   &parent_req);
			if (ret)
				return ret;

			best = parent_req.rate;
/*
 * IAMROOT, 2022.08.13:
 * - parent를 못바꾸면 parent clk을 가져오고, parent가 없으면 그냥 자기것을 가져온다.
 */
		} else if (parent) {
			best = clk_core_get_rate_nolock(parent);
		} else {
			best = clk_core_get_rate_nolock(core);
		}

		goto out;
	}

	/* find the parent that can provide the fastest rate <= rate */
	num_parents = core->num_parents;
	for (i = 0; i < num_parents; i++) {
		parent = clk_core_get_parent_by_index(core, i);
		if (!parent)
			continue;

/*
 * IAMROOT, 2022.08.13:
 * - 부모한테 영향을 줘야되는 경우. parent clk에서 재귀탐색을 하고,
 *   아니면 parent의 rate를 바로 가져온다.
 */
		if (core->flags & CLK_SET_RATE_PARENT) {
			parent_req = *req;
			ret = __clk_determine_rate(parent->hw, &parent_req);
			if (ret)
				continue;
		} else {
			parent_req.rate = clk_core_get_rate_nolock(parent);
		}

		if (mux_is_better_rate(req->rate, parent_req.rate,
				       best, flags)) {
			best_parent = parent;
			best = parent_req.rate;
		}
	}

	if (!best_parent)
		return -EINVAL;

out:
	if (best_parent)
		req->best_parent_hw = best_parent->hw;
	req->best_parent_rate = best;
	req->rate = best;

	return 0;
}
EXPORT_SYMBOL_GPL(clk_mux_determine_rate_flags);

struct clk *__clk_lookup(const char *name)
{
	struct clk_core *core = clk_core_lookup(name);

	return !core ? NULL : core->hw->clk;
}

/*
 * IAMROOT, 2022.08.13:
 * - clks_node(사용자들)에서 사용하는 값들과 clamp하며
 * - 가장큰 min과 가장 작은 max를 구한다.(교집합)
 */
static void clk_core_get_boundaries(struct clk_core *core,
				    unsigned long *min_rate,
				    unsigned long *max_rate)
{
	struct clk *clk_user;

	lockdep_assert_held(&prepare_lock);

	*min_rate = core->min_rate;
	*max_rate = core->max_rate;

	hlist_for_each_entry(clk_user, &core->clks, clks_node)
		*min_rate = max(*min_rate, clk_user->min_rate);

	hlist_for_each_entry(clk_user, &core->clks, clks_node)
		*max_rate = min(*max_rate, clk_user->max_rate);
}

void clk_hw_set_rate_range(struct clk_hw *hw, unsigned long min_rate,
			   unsigned long max_rate)
{
	hw->core->min_rate = min_rate;
	hw->core->max_rate = max_rate;
}
EXPORT_SYMBOL_GPL(clk_hw_set_rate_range);

/*
 * __clk_mux_determine_rate - clk_ops::determine_rate implementation for a mux type clk
 * @hw: mux type clk to determine rate on
 * @req: rate request, also used to return preferred parent and frequencies
 *
 * Helper for finding best parent to provide a given frequency. This can be used
 * directly as a determine_rate callback (e.g. for a mux), or from a more
 * complex clock that may combine a mux with other operations.
 *
 * Returns: 0 on success, -EERROR value on error
 */
/*
 * IAMROOT, 2022.08.13:
 * - @req에 최적인 rate를 구해온다.
 */
int __clk_mux_determine_rate(struct clk_hw *hw,
			     struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, 0);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate);

int __clk_mux_determine_rate_closest(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, CLK_MUX_ROUND_CLOSEST);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate_closest);

/***        clk api        ***/

static void clk_core_rate_unprotect(struct clk_core *core)
{
	lockdep_assert_held(&prepare_lock);

	if (!core)
		return;

	if (WARN(core->protect_count == 0,
	    "%s already unprotected\n", core->name))
		return;

	if (--core->protect_count > 0)
		return;

	clk_core_rate_unprotect(core->parent);
}

static int clk_core_rate_nuke_protect(struct clk_core *core)
{
	int ret;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return -EINVAL;

	if (core->protect_count == 0)
		return 0;

	ret = core->protect_count;
	core->protect_count = 1;
	clk_core_rate_unprotect(core);

	return ret;
}

/**
 * clk_rate_exclusive_put - release exclusivity over clock rate control
 * @clk: the clk over which the exclusivity is released
 *
 * clk_rate_exclusive_put() completes a critical section during which a clock
 * consumer cannot tolerate any other consumer making any operation on the
 * clock which could result in a rate change or rate glitch. Exclusive clocks
 * cannot have their rate changed, either directly or indirectly due to changes
 * further up the parent chain of clocks. As a result, clocks up parent chain
 * also get under exclusive control of the calling consumer.
 *
 * If exlusivity is claimed more than once on clock, even by the same consumer,
 * the rate effectively gets locked as exclusivity can't be preempted.
 *
 * Calls to clk_rate_exclusive_put() must be balanced with calls to
 * clk_rate_exclusive_get(). Calls to this function may sleep, and do not return
 * error status.
 */
void clk_rate_exclusive_put(struct clk *clk)
{
	if (!clk)
		return;

	clk_prepare_lock();

	/*
	 * if there is something wrong with this consumer protect count, stop
	 * here before messing with the provider
	 */
	if (WARN_ON(clk->exclusive_count <= 0))
		goto out;

	clk_core_rate_unprotect(clk->core);
	clk->exclusive_count--;
out:
	clk_prepare_unlock();
}
EXPORT_SYMBOL_GPL(clk_rate_exclusive_put);

static void clk_core_rate_protect(struct clk_core *core)
{
	lockdep_assert_held(&prepare_lock);

	if (!core)
		return;

	if (core->protect_count == 0)
		clk_core_rate_protect(core->parent);

	core->protect_count++;
}

static void clk_core_rate_restore_protect(struct clk_core *core, int count)
{
	lockdep_assert_held(&prepare_lock);

	if (!core)
		return;

	if (count == 0)
		return;

	clk_core_rate_protect(core);
	core->protect_count = count;
}

/**
 * clk_rate_exclusive_get - get exclusivity over the clk rate control
 * @clk: the clk over which the exclusity of rate control is requested
 *
 * clk_rate_exclusive_get() begins a critical section during which a clock
 * consumer cannot tolerate any other consumer making any operation on the
 * clock which could result in a rate change or rate glitch. Exclusive clocks
 * cannot have their rate changed, either directly or indirectly due to changes
 * further up the parent chain of clocks. As a result, clocks up parent chain
 * also get under exclusive control of the calling consumer.
 *
 * If exlusivity is claimed more than once on clock, even by the same consumer,
 * the rate effectively gets locked as exclusivity can't be preempted.
 *
 * Calls to clk_rate_exclusive_get() should be balanced with calls to
 * clk_rate_exclusive_put(). Calls to this function may sleep.
 * Returns 0 on success, -EERROR otherwise
 */
int clk_rate_exclusive_get(struct clk *clk)
{
	if (!clk)
		return 0;

	clk_prepare_lock();
	clk_core_rate_protect(clk->core);
	clk->exclusive_count++;
	clk_prepare_unlock();

	return 0;
}
EXPORT_SYMBOL_GPL(clk_rate_exclusive_get);

static void clk_core_unprepare(struct clk_core *core)
{
	lockdep_assert_held(&prepare_lock);

	if (!core)
		return;

	if (WARN(core->prepare_count == 0,
	    "%s already unprepared\n", core->name))
		return;

	if (WARN(core->prepare_count == 1 && core->flags & CLK_IS_CRITICAL,
	    "Unpreparing critical %s\n", core->name))
		return;

	if (core->flags & CLK_SET_RATE_GATE)
		clk_core_rate_unprotect(core);

	if (--core->prepare_count > 0)
		return;

	WARN(core->enable_count > 0, "Unpreparing enabled %s\n", core->name);

	trace_clk_unprepare(core);

	if (core->ops->unprepare)
		core->ops->unprepare(core->hw);

	clk_pm_runtime_put(core);

	trace_clk_unprepare_complete(core);
	clk_core_unprepare(core->parent);
}

static void clk_core_unprepare_lock(struct clk_core *core)
{
	clk_prepare_lock();
	clk_core_unprepare(core);
	clk_prepare_unlock();
}

/**
 * clk_unprepare - undo preparation of a clock source
 * @clk: the clk being unprepared
 *
 * clk_unprepare may sleep, which differentiates it from clk_disable.  In a
 * simple case, clk_unprepare can be used instead of clk_disable to gate a clk
 * if the operation may sleep.  One example is a clk which is accessed over
 * I2c.  In the complex case a clk gate operation may require a fast and a slow
 * part.  It is this reason that clk_unprepare and clk_disable are not mutually
 * exclusive.  In fact clk_disable must be called before clk_unprepare.
 */
void clk_unprepare(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return;

	clk_core_unprepare_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_unprepare);

/*
 * IAMROOT, 2022.08.13:
 * - 재귀적으로 parent까지 올라가며 prepare안되있으면 prepare 시킨다.
 *   실제 하드웨어적으로 clock을 on해주는 함수.
 */
static int clk_core_prepare(struct clk_core *core)
{
	int ret = 0;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return 0;

	if (core->prepare_count == 0) {
		ret = clk_pm_runtime_get(core);
		if (ret)
			return ret;

		ret = clk_core_prepare(core->parent);
		if (ret)
			goto runtime_put;

		trace_clk_prepare(core);

		if (core->ops->prepare)
			ret = core->ops->prepare(core->hw);

		trace_clk_prepare_complete(core);

		if (ret)
			goto unprepare;
	}

	core->prepare_count++;

	/*
	 * CLK_SET_RATE_GATE is a special case of clock protection
	 * Instead of a consumer claiming exclusive rate control, it is
	 * actually the provider which prevents any consumer from making any
	 * operation which could result in a rate change or rate glitch while
	 * the clock is prepared.
	 */
	if (core->flags & CLK_SET_RATE_GATE)
		clk_core_rate_protect(core);

	return 0;
unprepare:
	clk_core_unprepare(core->parent);
runtime_put:
	clk_pm_runtime_put(core);
	return ret;
}

/*
 * IAMROOT, 2022.08.13:
 * - clk prepare. (clk on)
 */
static int clk_core_prepare_lock(struct clk_core *core)
{
	int ret;

	clk_prepare_lock();
	ret = clk_core_prepare(core);
	clk_prepare_unlock();

	return ret;
}

/**
 * clk_prepare - prepare a clock source
 * @clk: the clk being prepared
 *
 * clk_prepare may sleep, which differentiates it from clk_enable.  In a simple
 * case, clk_prepare can be used instead of clk_enable to ungate a clk if the
 * operation may sleep.  One example is a clk which is accessed over I2c.  In
 * the complex case a clk ungate operation may require a fast and a slow part.
 * It is this reason that clk_prepare and clk_enable are not mutually
 * exclusive.  In fact clk_prepare must be called before clk_enable.
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_prepare(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_prepare_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_prepare);

static void clk_core_disable(struct clk_core *core)
{
	lockdep_assert_held(&enable_lock);

	if (!core)
		return;

	if (WARN(core->enable_count == 0, "%s already disabled\n", core->name))
		return;

	if (WARN(core->enable_count == 1 && core->flags & CLK_IS_CRITICAL,
	    "Disabling critical %s\n", core->name))
		return;

	if (--core->enable_count > 0)
		return;

	trace_clk_disable_rcuidle(core);

	if (core->ops->disable)
		core->ops->disable(core->hw);

	trace_clk_disable_complete_rcuidle(core);

	clk_core_disable(core->parent);
}

static void clk_core_disable_lock(struct clk_core *core)
{
	unsigned long flags;

	flags = clk_enable_lock();
	clk_core_disable(core);
	clk_enable_unlock(flags);
}

/**
 * clk_disable - gate a clock
 * @clk: the clk being gated
 *
 * clk_disable must not sleep, which differentiates it from clk_unprepare.  In
 * a simple case, clk_disable can be used instead of clk_unprepare to gate a
 * clk if the operation is fast and will never sleep.  One example is a
 * SoC-internal clk which is controlled via simple register writes.  In the
 * complex case a clk gate operation may require a fast and a slow part.  It is
 * this reason that clk_unprepare and clk_disable are not mutually exclusive.
 * In fact clk_disable must be called before clk_unprepare.
 */
void clk_disable(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return;

	clk_core_disable_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_disable);

/*
 * IAMROOT, 2022.08.13:
 * - gate clk enable. enable count가 안되있으면 parent까지 타고 올라가며
 *   enable 및 enable count증가 시킨다.
 */
static int clk_core_enable(struct clk_core *core)
{
	int ret = 0;

	lockdep_assert_held(&enable_lock);

	if (!core)
		return 0;

	if (WARN(core->prepare_count == 0,
	    "Enabling unprepared %s\n", core->name))
		return -ESHUTDOWN;

	if (core->enable_count == 0) {
/*
 * IAMROOT, 2022.08.13:
 * - parent까지 올라가 enable안되있다면 시킨다.
 */
		ret = clk_core_enable(core->parent);

		if (ret)
			return ret;

		trace_clk_enable_rcuidle(core);

		if (core->ops->enable)
			ret = core->ops->enable(core->hw);

		trace_clk_enable_complete_rcuidle(core);

		if (ret) {
			clk_core_disable(core->parent);
			return ret;
		}
	}

	core->enable_count++;
	return 0;
}

/*
 * IAMROOT, 2022.08.13:
 * - gate clk enable.
 */
static int clk_core_enable_lock(struct clk_core *core)
{
	unsigned long flags;
	int ret;

	flags = clk_enable_lock();
	ret = clk_core_enable(core);
	clk_enable_unlock(flags);

	return ret;
}

/**
 * clk_gate_restore_context - restore context for poweroff
 * @hw: the clk_hw pointer of clock whose state is to be restored
 *
 * The clock gate restore context function enables or disables
 * the gate clocks based on the enable_count. This is done in cases
 * where the clock context is lost and based on the enable_count
 * the clock either needs to be enabled/disabled. This
 * helps restore the state of gate clocks.
 */
void clk_gate_restore_context(struct clk_hw *hw)
{
	struct clk_core *core = hw->core;

	if (core->enable_count)
		core->ops->enable(hw);
	else
		core->ops->disable(hw);
}
EXPORT_SYMBOL_GPL(clk_gate_restore_context);

static int clk_core_save_context(struct clk_core *core)
{
	struct clk_core *child;
	int ret = 0;

	hlist_for_each_entry(child, &core->children, child_node) {
		ret = clk_core_save_context(child);
		if (ret < 0)
			return ret;
	}

	if (core->ops && core->ops->save_context)
		ret = core->ops->save_context(core->hw);

	return ret;
}

static void clk_core_restore_context(struct clk_core *core)
{
	struct clk_core *child;

	if (core->ops && core->ops->restore_context)
		core->ops->restore_context(core->hw);

	hlist_for_each_entry(child, &core->children, child_node)
		clk_core_restore_context(child);
}

/**
 * clk_save_context - save clock context for poweroff
 *
 * Saves the context of the clock register for powerstates in which the
 * contents of the registers will be lost. Occurs deep within the suspend
 * code.  Returns 0 on success.
 */
int clk_save_context(void)
{
	struct clk_core *clk;
	int ret;

	hlist_for_each_entry(clk, &clk_root_list, child_node) {
		ret = clk_core_save_context(clk);
		if (ret < 0)
			return ret;
	}

	hlist_for_each_entry(clk, &clk_orphan_list, child_node) {
		ret = clk_core_save_context(clk);
		if (ret < 0)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(clk_save_context);

/**
 * clk_restore_context - restore clock context after poweroff
 *
 * Restore the saved clock context upon resume.
 *
 */
void clk_restore_context(void)
{
	struct clk_core *core;

	hlist_for_each_entry(core, &clk_root_list, child_node)
		clk_core_restore_context(core);

	hlist_for_each_entry(core, &clk_orphan_list, child_node)
		clk_core_restore_context(core);
}
EXPORT_SYMBOL_GPL(clk_restore_context);

/**
 * clk_enable - ungate a clock
 * @clk: the clk being ungated
 *
 * clk_enable must not sleep, which differentiates it from clk_prepare.  In a
 * simple case, clk_enable can be used instead of clk_prepare to ungate a clk
 * if the operation will never sleep.  One example is a SoC-internal clk which
 * is controlled via simple register writes.  In the complex case a clk ungate
 * operation may require a fast and a slow part.  It is this reason that
 * clk_enable and clk_prepare are not mutually exclusive.  In fact clk_prepare
 * must be called before clk_enable.  Returns 0 on success, -EERROR
 * otherwise.
 */
int clk_enable(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_enable_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_enable);

/**
 * clk_is_enabled_when_prepared - indicate if preparing a clock also enables it.
 * @clk: clock source
 *
 * Returns true if clk_prepare() implicitly enables the clock, effectively
 * making clk_enable()/clk_disable() no-ops, false otherwise.
 *
 * This is of interest mainly to power management code where actually
 * disabling the clock also requires unpreparing it to have any material
 * effect.
 *
 * Regardless of the value returned here, the caller must always invoke
 * clk_enable() or clk_prepare_enable()  and counterparts for usage counts
 * to be right.
 */
bool clk_is_enabled_when_prepared(struct clk *clk)
{
	return clk && !(clk->core->ops->enable && clk->core->ops->disable);
}
EXPORT_SYMBOL_GPL(clk_is_enabled_when_prepared);

/*
 * IAMROOT, 2022.08.13:
 * - clk on 및 gate on
 */
static int clk_core_prepare_enable(struct clk_core *core)
{
	int ret;

	ret = clk_core_prepare_lock(core);
	if (ret)
		return ret;

	ret = clk_core_enable_lock(core);
	if (ret)
		clk_core_unprepare_lock(core);

	return ret;
}

static void clk_core_disable_unprepare(struct clk_core *core)
{
	clk_core_disable_lock(core);
	clk_core_unprepare_lock(core);
}

static void __init clk_unprepare_unused_subtree(struct clk_core *core)
{
	struct clk_core *child;

	lockdep_assert_held(&prepare_lock);

	hlist_for_each_entry(child, &core->children, child_node)
		clk_unprepare_unused_subtree(child);

	if (core->prepare_count)
		return;

	if (core->flags & CLK_IGNORE_UNUSED)
		return;

	if (clk_pm_runtime_get(core))
		return;

	if (clk_core_is_prepared(core)) {
		trace_clk_unprepare(core);
		if (core->ops->unprepare_unused)
			core->ops->unprepare_unused(core->hw);
		else if (core->ops->unprepare)
			core->ops->unprepare(core->hw);
		trace_clk_unprepare_complete(core);
	}

	clk_pm_runtime_put(core);
}

static void __init clk_disable_unused_subtree(struct clk_core *core)
{
	struct clk_core *child;
	unsigned long flags;

	lockdep_assert_held(&prepare_lock);

	hlist_for_each_entry(child, &core->children, child_node)
		clk_disable_unused_subtree(child);

	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_prepare_enable(core->parent);

	if (clk_pm_runtime_get(core))
		goto unprepare_out;

	flags = clk_enable_lock();

	if (core->enable_count)
		goto unlock_out;

	if (core->flags & CLK_IGNORE_UNUSED)
		goto unlock_out;

	/*
	 * some gate clocks have special needs during the disable-unused
	 * sequence.  call .disable_unused if available, otherwise fall
	 * back to .disable
	 */
	if (clk_core_is_enabled(core)) {
		trace_clk_disable(core);
		if (core->ops->disable_unused)
			core->ops->disable_unused(core->hw);
		else if (core->ops->disable)
			core->ops->disable(core->hw);
		trace_clk_disable_complete(core);
	}

unlock_out:
	clk_enable_unlock(flags);
	clk_pm_runtime_put(core);
unprepare_out:
	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_disable_unprepare(core->parent);
}

static bool clk_ignore_unused __initdata;
static int __init clk_ignore_unused_setup(char *__unused)
{
	clk_ignore_unused = true;
	return 1;
}
__setup("clk_ignore_unused", clk_ignore_unused_setup);

static int __init clk_disable_unused(void)
{
	struct clk_core *core;

	if (clk_ignore_unused) {
		pr_warn("clk: Not disabling unused clocks\n");
		return 0;
	}

	clk_prepare_lock();

	hlist_for_each_entry(core, &clk_root_list, child_node)
		clk_disable_unused_subtree(core);

	hlist_for_each_entry(core, &clk_orphan_list, child_node)
		clk_disable_unused_subtree(core);

	hlist_for_each_entry(core, &clk_root_list, child_node)
		clk_unprepare_unused_subtree(core);

	hlist_for_each_entry(core, &clk_orphan_list, child_node)
		clk_unprepare_unused_subtree(core);

	clk_prepare_unlock();

	return 0;
}
late_initcall_sync(clk_disable_unused);

/*
 * IAMROOT, 2022.08.13:
 * - determin or round
 */
static int clk_core_determine_round_nolock(struct clk_core *core,
					   struct clk_rate_request *req)
{
	long rate;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return 0;

	/*
	 * At this point, core protection will be disabled
	 * - if the provider is not protected at all
	 * - if the calling consumer is the only one which has exclusivity
	 *   over the provider
	 */
/*
 * IAMROOT, 2022.08.13:
 * - protected면 core, > determine_rate > round_rate를 우선순위로 설정한다.
 *   ex) __clk_mux_determine_rate,
 */
	if (clk_core_rate_is_protected(core)) {
		req->rate = core->rate;
	} else if (core->ops->determine_rate) {
		return core->ops->determine_rate(core->hw, req);
	} else if (core->ops->round_rate) {
		rate = core->ops->round_rate(core->hw, req->rate,
					     &req->best_parent_rate);
		if (rate < 0)
			return rate;

		req->rate = rate;
	} else {
		return -EINVAL;
	}

	return 0;
}

/*
 * IAMROOT, 2022.08.13:
 * - @parent의 hw, rate를 그대로 가져온다.
 */
static void clk_core_init_rate_req(struct clk_core * const core,
				   struct clk_rate_request *req)
{
	struct clk_core *parent;

	if (WARN_ON(!core || !req))
		return;

	parent = core->parent;
	if (parent) {
		req->best_parent_hw = parent->hw;
		req->best_parent_rate = parent->rate;
	} else {
		req->best_parent_hw = NULL;
		req->best_parent_rate = 0;
	}
}

/*
 * IAMROOT, 2022.08.13:
 * - ops 함수 제공 확인.
 */
static bool clk_core_can_round(struct clk_core * const core)
{
	return core->ops->determine_rate || core->ops->round_rate;
}

/*
 * IAMROOT, 2022.08.13:
 * - @req에 맞는 rate를 구해온다. 
 */
static int clk_core_round_rate_nolock(struct clk_core *core,
				      struct clk_rate_request *req)
{
	lockdep_assert_held(&prepare_lock);

	if (!core) {
		req->rate = 0;
		return 0;
	}

	clk_core_init_rate_req(core, req);

/*
 * IAMROOT, 2022.08.13:
 * - @core에서 detertmin이나 round를 할수없는경우, parent 변경 가능한 경우
 *   parent에서 round를 시도한다.
 *   그것도 안되면 자기것을 그냥 가져온다.
 */
	if (clk_core_can_round(core))
		return clk_core_determine_round_nolock(core, req);
	else if (core->flags & CLK_SET_RATE_PARENT)
		return clk_core_round_rate_nolock(core->parent, req);

	req->rate = core->rate;
	return 0;
}

/**
 * __clk_determine_rate - get the closest rate actually supported by a clock
 * @hw: determine the rate of this clock
 * @req: target rate request
 *
 * Useful for clk_ops such as .set_rate and .determine_rate.
 */
/*
 * IAMROOT, 2022.08.13:
 * - 
 */
int __clk_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
/*
 * IAMROOT, 2022.08.13:
 * - 재귀호출로 사용할수도있다. 최상위까지 왓을 경우에 대한 탈출조건.
 */
	if (!hw) {
		req->rate = 0;
		return 0;
	}

	return clk_core_round_rate_nolock(hw->core, req);
}
EXPORT_SYMBOL_GPL(__clk_determine_rate);

/**
 * clk_hw_round_rate() - round the given rate for a hw clk
 * @hw: the hw clk for which we are rounding a rate
 * @rate: the rate which is to be rounded
 *
 * Takes in a rate as input and rounds it to a rate that the clk can actually
 * use.
 *
 * Context: prepare_lock must be held.
 *          For clk providers to call from within clk_ops such as .round_rate,
 *          .determine_rate.
 *
 * Return: returns rounded rate of hw clk if clk supports round_rate operation
 *         else returns the parent rate.
 */
unsigned long clk_hw_round_rate(struct clk_hw *hw, unsigned long rate)
{
	int ret;
	struct clk_rate_request req;

	clk_core_get_boundaries(hw->core, &req.min_rate, &req.max_rate);
	req.rate = rate;

	ret = clk_core_round_rate_nolock(hw->core, &req);
	if (ret)
		return 0;

	return req.rate;
}
EXPORT_SYMBOL_GPL(clk_hw_round_rate);

/**
 * clk_round_rate - round the given rate for a clk
 * @clk: the clk for which we are rounding a rate
 * @rate: the rate which is to be rounded
 *
 * Takes in a rate as input and rounds it to a rate that the clk can actually
 * use which is then returned.  If clk doesn't support round_rate operation
 * then the parent rate is returned.
 */

/*
 * IAMROOT, 2022.08.13:
 * - @rate에 근접한 실제 사용가능할수있는 clk rate를 구해온다.
 */
long clk_round_rate(struct clk *clk, unsigned long rate)
{
	struct clk_rate_request req;
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

/*
 * IAMROOT, 2022.08.13:
 * - clk을 사용하는 모든 사용자들의 범위 이내로 min, max를 clamp 시킨다.
 */
	clk_core_get_boundaries(clk->core, &req.min_rate, &req.max_rate);
	req.rate = rate;

/*
 * IAMROOT, 2022.08.13:
 * - rate를 설정할수있는지 확인한다.
 */
	ret = clk_core_round_rate_nolock(clk->core, &req);

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	if (ret)
		return ret;

	return req.rate;
}
EXPORT_SYMBOL_GPL(clk_round_rate);

/**
 * __clk_notify - call clk notifier chain
 * @core: clk that is changing rate
 * @msg: clk notifier type (see include/linux/clk.h)
 * @old_rate: old clk rate
 * @new_rate: new clk rate
 *
 * Triggers a notifier call chain on the clk rate-change notification
 * for 'clk'.  Passes a pointer to the struct clk and the previous
 * and current rates to the notifier callback.  Intended to be called by
 * internal clock code only.  Returns NOTIFY_DONE from the last driver
 * called if all went well, or NOTIFY_STOP or NOTIFY_BAD immediately if
 * a driver returns that.
 */
/*
 * IAMROOT, 2022.08.13:
 * - notifier에 등록된 clk이 가리키는 core에만 notify한다.
 * 참고) dw8250_clk_notifier_cb, gcc_ipq4019_cpu_clk_notifier_fn
 */
static int __clk_notify(struct clk_core *core, unsigned long msg,
		unsigned long old_rate, unsigned long new_rate)
{
	struct clk_notifier *cn;
	struct clk_notifier_data cnd;
	int ret = NOTIFY_DONE;

	cnd.old_rate = old_rate;
	cnd.new_rate = new_rate;

	list_for_each_entry(cn, &clk_notifier_list, node) {
		if (cn->clk->core == core) {
			cnd.clk = cn->clk;
			ret = srcu_notifier_call_chain(&cn->notifier_head, msg,
					&cnd);
			if (ret & NOTIFY_STOP_MASK)
				return ret;
		}
	}

	return ret;
}

/**
 * __clk_recalc_accuracies
 * @core: first clk in the subtree
 *
 * Walks the subtree of clks starting with clk and recalculates accuracies as
 * it goes.  Note that if a clk does not implement the .recalc_accuracy
 * callback then it is assumed that the clock will take on the accuracy of its
 * parent.
 */
static void __clk_recalc_accuracies(struct clk_core *core)
{
	unsigned long parent_accuracy = 0;
	struct clk_core *child;

	lockdep_assert_held(&prepare_lock);

	if (core->parent)
		parent_accuracy = core->parent->accuracy;

	if (core->ops->recalc_accuracy)
		core->accuracy = core->ops->recalc_accuracy(core->hw,
							  parent_accuracy);
	else
		core->accuracy = parent_accuracy;

	hlist_for_each_entry(child, &core->children, child_node)
		__clk_recalc_accuracies(child);
}

static long clk_core_get_accuracy_recalc(struct clk_core *core)
{
	if (core && (core->flags & CLK_GET_ACCURACY_NOCACHE))
		__clk_recalc_accuracies(core);

	return clk_core_get_accuracy_no_lock(core);
}

/**
 * clk_get_accuracy - return the accuracy of clk
 * @clk: the clk whose accuracy is being returned
 *
 * Simply returns the cached accuracy of the clk, unless
 * CLK_GET_ACCURACY_NOCACHE flag is set, which means a recalc_rate will be
 * issued.
 * If clk is NULL then returns 0.
 */
long clk_get_accuracy(struct clk *clk)
{
	long accuracy;

	if (!clk)
		return 0;

	clk_prepare_lock();
	accuracy = clk_core_get_accuracy_recalc(clk->core);
	clk_prepare_unlock();

	return accuracy;
}
EXPORT_SYMBOL_GPL(clk_get_accuracy);

/*
 * IAMROOT, 2022.08.13:
 * - parent로 일단 rate로 해놓고, ops가 등록되있으면 ops로 rate를 재산출한다.
 */
static unsigned long clk_recalc(struct clk_core *core,
				unsigned long parent_rate)
{
	unsigned long rate = parent_rate;

	if (core->ops->recalc_rate && !clk_pm_runtime_get(core)) {
		rate = core->ops->recalc_rate(core->hw, parent_rate);
		clk_pm_runtime_put(core);
	}
	return rate;
}

/**
 * __clk_recalc_rates
 * @core: first clk in the subtree
 * @msg: notification type (see include/linux/clk.h)
 *
 * Walks the subtree of clks starting with clk and recalculates rates as it
 * goes.  Note that if a clk does not implement the .recalc_rate callback then
 * it is assumed that the clock will take on the rate of its parent.
 *
 * clk_recalc_rates also propagates the POST_RATE_CHANGE notification,
 * if necessary.
 */
static void __clk_recalc_rates(struct clk_core *core, unsigned long msg)
{
	unsigned long old_rate;
	unsigned long parent_rate = 0;
	struct clk_core *child;

	lockdep_assert_held(&prepare_lock);

	old_rate = core->rate;

	if (core->parent)
		parent_rate = core->parent->rate;

	core->rate = clk_recalc(core, parent_rate);

	/*
	 * ignore NOTIFY_STOP and NOTIFY_BAD return values for POST_RATE_CHANGE
	 * & ABORT_RATE_CHANGE notifiers
	 */
	if (core->notifier_count && msg)
		__clk_notify(core, msg, old_rate, core->rate);

	hlist_for_each_entry(child, &core->children, child_node)
		__clk_recalc_rates(child, msg);
}

static unsigned long clk_core_get_rate_recalc(struct clk_core *core)
{
	if (core && (core->flags & CLK_GET_RATE_NOCACHE))
		__clk_recalc_rates(core, 0);

	return clk_core_get_rate_nolock(core);
}

/**
 * clk_get_rate - return the rate of clk
 * @clk: the clk whose rate is being returned
 *
 * Simply returns the cached rate of the clk, unless CLK_GET_RATE_NOCACHE flag
 * is set, which means a recalc_rate will be issued.
 * If clk is NULL then returns 0.
 */
unsigned long clk_get_rate(struct clk *clk)
{
	unsigned long rate;

	if (!clk)
		return 0;

	clk_prepare_lock();
	rate = clk_core_get_rate_recalc(clk->core);
	clk_prepare_unlock();

	return rate;
}
EXPORT_SYMBOL_GPL(clk_get_rate);

/*
 * IAMROOT, 2022.08.06:
 * - @core의 parent중에서 @parent가 있는지 확인한다.
 *   1. core->parent[i].core와 parent가 같거나.
 *   core->parent[i].core가 없는 것들 중에서.
 *   1. hw가 같거나.
 *   2. clk_core_get(core, i)와 core가 같거나
 *   3. core->parents의 이름과 parent의 이름이 같은경우.
 */
static int clk_fetch_parent_index(struct clk_core *core,
				  struct clk_core *parent)
{
	int i;

	if (!parent)
		return -EINVAL;

	for (i = 0; i < core->num_parents; i++) {
		/* Found it first try! */
		if (core->parents[i].core == parent)
			return i;

		/* Something else is here, so keep looking */
		if (core->parents[i].core)
			continue;

		/* Maybe core hasn't been cached but the hw is all we know? */
		if (core->parents[i].hw) {
			if (core->parents[i].hw == parent->hw)
				break;

			/* Didn't match, but we're expecting a clk_hw */
			continue;
		}

		/* Maybe it hasn't been cached (clk_set_parent() path) */
		if (parent == clk_core_get(core, i))
			break;

		/* Fallback to comparing globally unique names */
		if (core->parents[i].name &&
		    !strcmp(parent->name, core->parents[i].name))
			break;
	}

	if (i == core->num_parents)
		return -EINVAL;

	core->parents[i].core = parent;
	return i;
}

/**
 * clk_hw_get_parent_index - return the index of the parent clock
 * @hw: clk_hw associated with the clk being consumed
 *
 * Fetches and returns the index of parent clock. Returns -EINVAL if the given
 * clock does not have a current parent.
 */
int clk_hw_get_parent_index(struct clk_hw *hw)
{
	struct clk_hw *parent = clk_hw_get_parent(hw);

	if (WARN_ON(parent == NULL))
		return -EINVAL;

	return clk_fetch_parent_index(hw->core, parent->core);
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent_index);

/*
 * Update the orphan status of @core and all its children.
 */
static void clk_core_update_orphan_status(struct clk_core *core, bool is_orphan)
{
	struct clk_core *child;

	core->orphan = is_orphan;

	hlist_for_each_entry(child, &core->children, child_node)
		clk_core_update_orphan_status(child, is_orphan);
}

/*
 * IAMROOT, 2022.08.13:
 * - @new_parent가 있으면 core를 new_parent orphan상태로 일치시키고 new_parent의
 *   child로 변경한다.
 * - @new_parent가 없으면 parent가 없어진 것이므로 orphan으로 변경한다.
 */
static void clk_reparent(struct clk_core *core, struct clk_core *new_parent)
{
	bool was_orphan = core->orphan;

	hlist_del(&core->child_node);

	if (new_parent) {
/*
 * IAMROOT, 2022.08.13:
 * - 둘중하나가 orphan 상태가 다르면 core의 orphan상태를 new_parent의 상태로 업데이트
 */
		bool becomes_orphan = new_parent->orphan;

		/* avoid duplicate POST_RATE_CHANGE notifications */
		if (new_parent->new_child == core)
			new_parent->new_child = NULL;

		hlist_add_head(&core->child_node, &new_parent->children);

		if (was_orphan != becomes_orphan)
			clk_core_update_orphan_status(core, becomes_orphan);
	} else {
		hlist_add_head(&core->child_node, &clk_orphan_list);
		if (!was_orphan)
			clk_core_update_orphan_status(core, true);
	}

	core->parent = new_parent;
}

/*
 * IAMROOT, 2022.08.13:
 * @return old_parent
 * - @core의 parent를 @parent로 변경한다.
 */
static struct clk_core *__clk_set_parent_before(struct clk_core *core,
					   struct clk_core *parent)
{
	unsigned long flags;
	struct clk_core *old_parent = core->parent;

	/*
	 * 1. enable parents for CLK_OPS_PARENT_ENABLE clock
	 *
	 * 2. Migrate prepare state between parents and prevent race with
	 * clk_enable().
	 *
	 * If the clock is not prepared, then a race with
	 * clk_enable/disable() is impossible since we already have the
	 * prepare lock (future calls to clk_enable() need to be preceded by
	 * a clk_prepare()).
	 *
	 * If the clock is prepared, migrate the prepared state to the new
	 * parent and also protect against a race with clk_enable() by
	 * forcing the clock and the new parent on.  This ensures that all
	 * future calls to clk_enable() are practically NOPs with respect to
	 * hardware and software states.
	 *
	 * See also: Comment for clk_set_parent() below.
	 */

/*
 * IAMROOT, 2022.08.13:
 * - CLK_OPS_PARENT_ENABLE이 있는경우 old_parent와 parent를 prepare/enable 한다.
 */
	/* enable old_parent & parent if CLK_OPS_PARENT_ENABLE is set */
	if (core->flags & CLK_OPS_PARENT_ENABLE) {
		clk_core_prepare_enable(old_parent);
		clk_core_prepare_enable(parent);
	}

	/* migrate prepare count if > 0 */
/*
 * IAMROOT, 2022.08.13:
 * - 현재 core가 prepare 되있는 상태라면 parent를 prepare/enable하고 core도 enable한다.
 */
	if (core->prepare_count) {
		clk_core_prepare_enable(parent);
		clk_core_enable_lock(core);
	}

	/* update the clk tree topology */
	flags = clk_enable_lock();
	clk_reparent(core, parent);
	clk_enable_unlock(flags);

	return old_parent;
}

/*
 * IAMROOT, 2022.08.13:
 * - 필요없어진 clk을 diable/unprepare
 */
static void __clk_set_parent_after(struct clk_core *core,
				   struct clk_core *parent,
				   struct clk_core *old_parent)
{
	/*
	 * Finish the migration of prepare state and undo the changes done
	 * for preventing a race with clk_enable().
	 */
	if (core->prepare_count) {
		clk_core_disable_lock(core);
		clk_core_disable_unprepare(old_parent);
	}

	/* re-balance ref counting if CLK_OPS_PARENT_ENABLE is set */
	if (core->flags & CLK_OPS_PARENT_ENABLE) {
		clk_core_disable_unprepare(parent);
		clk_core_disable_unprepare(old_parent);
	}
}

static int __clk_set_parent(struct clk_core *core, struct clk_core *parent,
			    u8 p_index)
{
	unsigned long flags;
	int ret = 0;
	struct clk_core *old_parent;

	old_parent = __clk_set_parent_before(core, parent);

	trace_clk_set_parent(core, parent);

	/* change clock input source */
	if (parent && core->ops->set_parent)
		ret = core->ops->set_parent(core->hw, p_index);

	trace_clk_set_parent_complete(core, parent);

	if (ret) {
		flags = clk_enable_lock();
		clk_reparent(core, old_parent);
		clk_enable_unlock(flags);
		__clk_set_parent_after(core, old_parent, parent);

		return ret;
	}

	__clk_set_parent_after(core, parent, old_parent);

	return 0;
}

/**
 * __clk_speculate_rates
 * @core: first clk in the subtree
 * @parent_rate: the "future" rate of clk's parent
 *
 * Walks the subtree of clks starting with clk, speculating rates as it
 * goes and firing off PRE_RATE_CHANGE notifications as necessary.
 *
 * Unlike clk_recalc_rates, clk_speculate_rates exists only for sending
 * pre-rate change notifications and returns early if no clks in the
 * subtree have subscribed to the notifications.  Note that if a clk does not
 * implement the .recalc_rate callback then it is assumed that the clock will
 * take on the rate of its parent.
 */
static int __clk_speculate_rates(struct clk_core *core,
				 unsigned long parent_rate)
{
	struct clk_core *child;
	unsigned long new_rate;
	int ret = NOTIFY_DONE;

	lockdep_assert_held(&prepare_lock);

	new_rate = clk_recalc(core, parent_rate);

	/* abort rate change if a driver returns NOTIFY_BAD or NOTIFY_STOP */
	if (core->notifier_count)
		ret = __clk_notify(core, PRE_RATE_CHANGE, core->rate, new_rate);

	if (ret & NOTIFY_STOP_MASK) {
		pr_debug("%s: clk notifier callback for clock %s aborted with error %d\n",
				__func__, core->name, ret);
		goto out;
	}

	hlist_for_each_entry(child, &core->children, child_node) {
		ret = __clk_speculate_rates(child, new_rate);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}

out:
	return ret;
}

/*
 * IAMROOT, 2022.08.13:
 * - 결정된 new_rate, new_parent, p_index을 subtree 재귀로 넣어준다.
 */
static void clk_calc_subtree(struct clk_core *core, unsigned long new_rate,
			     struct clk_core *new_parent, u8 p_index)
{
	struct clk_core *child;

	core->new_rate = new_rate;
	core->new_parent = new_parent;
	core->new_parent_index = p_index;
	/* include clk in new parent's PRE_RATE_CHANGE notifications */
	core->new_child = NULL;
/*
 * IAMROOT, 2022.08.13:
 * - 현재 parent랑 같이 않으면 child도 이동을 해야되므로 new_child로 set해놓는다.
 */
	if (new_parent && new_parent != core->parent)
		new_parent->new_child = core;

/*
 * IAMROOT, 2022.08.13:
 * - chlid 가 parent가 되어 재귀로 new를 설정하러 간다.
 */
	hlist_for_each_entry(child, &core->children, child_node) {
		child->new_rate = clk_recalc(child, new_rate);
		clk_calc_subtree(child, child->new_rate, NULL, 0);
	}
}

/*
 * calculate the new rates returning the topmost clock that has to be
 * changed.
 */
/*
 * IAMROOT, 2022.08.13:
 * - tree를 재귀로 돌며 @rate를 설정할수있는 new_rate, new_parent, new_child를
 *   찾아서 얻어온다.
 */
static struct clk_core *clk_calc_new_rates(struct clk_core *core,
					   unsigned long rate)
{
	struct clk_core *top = core;
	struct clk_core *old_parent, *parent;
	unsigned long best_parent_rate = 0;
	unsigned long new_rate;
	unsigned long min_rate;
	unsigned long max_rate;
	int p_index = 0;
	long ret;

	/* sanity */
	if (IS_ERR_OR_NULL(core))
		return NULL;

	/* save parent rate, if it exists */
	parent = old_parent = core->parent;
	if (parent)
		best_parent_rate = parent->rate;

	clk_core_get_boundaries(core, &min_rate, &max_rate);

	/* find the closest rate and parent clk/rate */
/*
 * IAMROOT, 2022.08.13:
 * - round로 구해올수있으면 round기능으로 rate를 구해온다.
 */
	if (clk_core_can_round(core)) {
		struct clk_rate_request req;

		req.rate = rate;
		req.min_rate = min_rate;
		req.max_rate = max_rate;

		clk_core_init_rate_req(core, &req);

		ret = clk_core_determine_round_nolock(core, &req);
		if (ret < 0)
			return NULL;

		best_parent_rate = req.best_parent_rate;
		new_rate = req.rate;
		parent = req.best_parent_hw ? req.best_parent_hw->core : NULL;

		if (new_rate < min_rate || new_rate > max_rate)
			return NULL;
	} else if (!parent || !(core->flags & CLK_SET_RATE_PARENT)) {
/*
 * IAMROOT, 2022.08.13:
 * - parent가 없거나 parent가 있어도 parent로 올라갈수없으면 자기것으로 설정후 return.
 */
		/* pass-through clock without adjustable parent */
		core->new_rate = core->rate;
		return NULL;
	} else {
/*
 * IAMROOT, 2022.08.13:
 * - parent가 있고 CLK_SET_RATE_PARENT가 가능한 상태. 재귀로 parent에서 시도해본다.
 */
		/* pass-through clock with adjustable parent */
		top = clk_calc_new_rates(parent, rate);
		new_rate = parent->new_rate;
		goto out;
	}

	/* some clocks must be gated to change parent */
/*
 * IAMROOT, 2022.08.13:
 * - gate가 열려 있는 상태. 사용중이 상태이기때문에 바꾸지 못한다.
 */
	if (parent != old_parent &&
	    (core->flags & CLK_SET_PARENT_GATE) && core->prepare_count) {
		pr_debug("%s: %s not gated but wants to reparent\n",
			 __func__, core->name);
		return NULL;
	}

	/* try finding the new parent index */
/*
 * IAMROOT, 2022.08.13:
 * - parent가 여러개인경우 @parent의 index를 구해온다.
 */
	if (parent && core->num_parents > 1) {
		p_index = clk_fetch_parent_index(core, parent);
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
				 __func__, parent->name, core->name);
			return NULL;
		}
	}

/*
 * IAMROOT, 2022.08.13:
 * - parent rate랑 best랑 일치하지 않으면 best로 변경을 시도한다.(재귀)
 */
	if ((core->flags & CLK_SET_RATE_PARENT) && parent &&
	    best_parent_rate != parent->rate)
		top = clk_calc_new_rates(parent, best_parent_rate);

out:
	clk_calc_subtree(core, new_rate, parent, p_index);

	return top;
}

/*
 * Notify about rate changes in a subtree. Always walk down the whole tree
 * so that in case of an error we can walk down the whole tree again and
 * abort the change.
 */
/*
 * IAMROOT, 2022.08.13:
 * @event PRE_RATE_CHANGE등.
 * - 재귀로 @event를 전달한다.
 */
static struct clk_core *clk_propagate_rate_change(struct clk_core *core,
						  unsigned long event)
{
	struct clk_core *child, *tmp_clk, *fail_clk = NULL;
	int ret = NOTIFY_DONE;

	if (core->rate == core->new_rate)
		return NULL;

	if (core->notifier_count) {
/*
 * IAMROOT, 2022.08.13:
 * - clk notifier들한테 전달한다.
 */
		ret = __clk_notify(core, event, core->rate, core->new_rate);
		if (ret & NOTIFY_STOP_MASK)
			fail_clk = core;
	}

/*
 * IAMROOT, 2022.08.13:
 * - child한태도 event를 전달한다.
 */
	hlist_for_each_entry(child, &core->children, child_node) {
		/* Skip children who will be reparented to another clock */
		if (child->new_parent && child->new_parent != core)
			continue;
		tmp_clk = clk_propagate_rate_change(child, event);
		if (tmp_clk)
			fail_clk = tmp_clk;
	}

	/* handle the new child who might not be in core->children yet */
/*
 * IAMROOT, 2022.08.13:
 * - new child한테도 보낸다.
 */
	if (core->new_child) {
		tmp_clk = clk_propagate_rate_change(core->new_child, event);
		if (tmp_clk)
			fail_clk = tmp_clk;
	}

	return fail_clk;
}

/*
 * walk down a subtree and set the new rates notifying the rate
 * change on the way
 */
/*
 * IAMROOT, 2022.08.13:
 * - 실제 parent 변경 및 하드웨어적인 clk on, rate 설정.
 *   재귀로 child를 순회하며 조작한다.
 */
static void clk_change_rate(struct clk_core *core)
{
	struct clk_core *child;
	struct hlist_node *tmp;
	unsigned long old_rate;
	unsigned long best_parent_rate = 0;
	bool skip_set_rate = false;
	struct clk_core *old_parent;
	struct clk_core *parent = NULL;

	old_rate = core->rate;

/*
 * IAMROOT, 2022.08.13:
 * - new_parent가 있으면 new_parent를 사용. 아니면 parent를 사용.
 */
	if (core->new_parent) {
		parent = core->new_parent;
		best_parent_rate = core->new_parent->rate;
	} else if (core->parent) {
		parent = core->parent;
		best_parent_rate = core->parent->rate;
	}

	if (clk_pm_runtime_get(core))
		return;

	if (core->flags & CLK_SET_RATE_UNGATE) {
		clk_core_prepare(core);
		clk_core_enable_lock(core);
	}

/*
 * IAMROOT, 2022.08.13:
 * - mux clk에 대한 조작.
 */
	if (core->new_parent && core->new_parent != core->parent) {
/*
 * IAMROOT, 2022.08.13:
 * - @core의 parent을 실제적으로 변경한다.
 */
		old_parent = __clk_set_parent_before(core, core->new_parent);
		trace_clk_set_parent(core, core->new_parent);

/*
 * IAMROOT, 2022.08.13:
 * - 현재 clk의 parent(입력소스) 및 rate를 변경한다.
 */
		if (core->ops->set_rate_and_parent) {
			skip_set_rate = true;
			core->ops->set_rate_and_parent(core->hw, core->new_rate,
					best_parent_rate,
					core->new_parent_index);
		} else if (core->ops->set_parent) {
			core->ops->set_parent(core->hw, core->new_parent_index);
		}

		trace_clk_set_parent_complete(core, core->new_parent);
		__clk_set_parent_after(core, core->new_parent, old_parent);
	}

	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_prepare_enable(parent);

	trace_clk_set_rate(core, core->new_rate);

/*
 * IAMROOT, 2022.08.13:
 * - rate clock 조작.
 */
	if (!skip_set_rate && core->ops->set_rate)
		core->ops->set_rate(core->hw, core->new_rate, best_parent_rate);

	trace_clk_set_rate_complete(core, core->new_rate);

/*
 * IAMROOT, 2022.08.13:
 * - 최종 rate를 산출한다.
 */
	core->rate = clk_recalc(core, best_parent_rate);

	if (core->flags & CLK_SET_RATE_UNGATE) {
		clk_core_disable_lock(core);
		clk_core_unprepare(core);
	}

	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_disable_unprepare(parent);

/*
 * IAMROOT, 2022.08.13:
 * - 실제 적용.
 * - 모든 clk설정에 문제가 없으면 적용한다.
 */
	if (core->notifier_count && old_rate != core->rate)
		__clk_notify(core, POST_RATE_CHANGE, old_rate, core->rate);

	if (core->flags & CLK_RECALC_NEW_RATES)
		(void)clk_calc_new_rates(core, core->new_rate);

	/*
	 * Use safe iteration, as change_rate can actually swap parents
	 * for certain clock types.
	 */
/*
 * IAMROOT, 2022.08.13:
 * - 재귀로 child를 순회하며 변경한다.
 */
	hlist_for_each_entry_safe(child, tmp, &core->children, child_node) {
		/* Skip children who will be reparented to another clock */
		if (child->new_parent && child->new_parent != core)
			continue;
		clk_change_rate(child);
	}

	/* handle the new child who might not be in core->children yet */
	if (core->new_child)
		clk_change_rate(core->new_child);

	clk_pm_runtime_put(core);
}

static unsigned long clk_core_req_round_rate_nolock(struct clk_core *core,
						     unsigned long req_rate)
{
	int ret, cnt;
	struct clk_rate_request req;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return 0;

	/* simulate what the rate would be if it could be freely set */
	cnt = clk_core_rate_nuke_protect(core);
	if (cnt < 0)
		return cnt;

	clk_core_get_boundaries(core, &req.min_rate, &req.max_rate);
	req.rate = req_rate;

	ret = clk_core_round_rate_nolock(core, &req);

	/* restore the protection */
	clk_core_rate_restore_protect(core, cnt);

	return ret ? 0 : req.rate;
}

static int clk_core_set_rate_nolock(struct clk_core *core,
				    unsigned long req_rate)
{
	struct clk_core *top, *fail_clk;
	unsigned long rate;
	int ret = 0;

	if (!core)
		return 0;

	rate = clk_core_req_round_rate_nolock(core, req_rate);

/*
 * IAMROOT, 2022.08.13:
 * - @core의 현재 rate가 rate랑 일치하면 return.
 */
	/* bail early if nothing to do */
	if (rate == clk_core_get_rate_nolock(core))
		return 0;

	/* fail on a direct rate set of a protected provider */
	if (clk_core_rate_is_protected(core))
		return -EBUSY;

	/* calculate new rates and get the topmost changed clock */
/*
 * IAMROOT, 2022.08.13:
 * - @req_rate를 set할수있는 new 값들을 구해온다.
 */
	top = clk_calc_new_rates(core, req_rate);
	if (!top)
		return -EINVAL;

	ret = clk_pm_runtime_get(core);
	if (ret)
		return ret;

/*
 * IAMROOT, 2022.08.13:
 * - 이전에 구한 new값으로 변경 가능한지 물어본다.(실제 설정은 안한다.) 안되면 롤백.
 */
	/* notify that we are about to change rates */
	fail_clk = clk_propagate_rate_change(top, PRE_RATE_CHANGE);
	if (fail_clk) {
		pr_debug("%s: failed to set %s rate\n", __func__,
				fail_clk->name);
		clk_propagate_rate_change(top, ABORT_RATE_CHANGE);
		ret = -EBUSY;
		goto err;
	}

/*
 * IAMROOT, 2022.08.13:
 * - 잘 바뀌면. 실제 change
 */
	/* change the rates */
	clk_change_rate(top);

	core->req_rate = req_rate;
err:
	clk_pm_runtime_put(core);

	return ret;
}

/**
 * clk_set_rate - specify a new rate for clk
 * @clk: the clk whose rate is being changed
 * @rate: the new rate for clk
 *
 * In the simplest case clk_set_rate will only adjust the rate of clk.
 *
 * Setting the CLK_SET_RATE_PARENT flag allows the rate change operation to
 * propagate up to clk's parent; whether or not this happens depends on the
 * outcome of clk's .round_rate implementation.  If *parent_rate is unchanged
 * after calling .round_rate then upstream parent propagation is ignored.  If
 * *parent_rate comes back with a new rate for clk's parent then we propagate
 * up to clk's parent and set its rate.  Upward propagation will continue
 * until either a clk does not support the CLK_SET_RATE_PARENT flag or
 * .round_rate stops requesting changes to clk's parent_rate.
 *
 * Rate changes are accomplished via tree traversal that also recalculates the
 * rates for the clocks and fires off POST_RATE_CHANGE notifiers.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;

	if (!clk)
		return 0;

	/* prevent racing with updates to the clock topology */
	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

	ret = clk_core_set_rate_nolock(clk->core, rate);

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate);

/**
 * clk_set_rate_exclusive - specify a new rate and get exclusive control
 * @clk: the clk whose rate is being changed
 * @rate: the new rate for clk
 *
 * This is a combination of clk_set_rate() and clk_rate_exclusive_get()
 * within a critical section
 *
 * This can be used initially to ensure that at least 1 consumer is
 * satisfied when several consumers are competing for exclusivity over the
 * same clock provider.
 *
 * The exclusivity is not applied if setting the rate failed.
 *
 * Calls to clk_rate_exclusive_get() should be balanced with calls to
 * clk_rate_exclusive_put().
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_set_rate_exclusive(struct clk *clk, unsigned long rate)
{
	int ret;

	if (!clk)
		return 0;

	/* prevent racing with updates to the clock topology */
	clk_prepare_lock();

	/*
	 * The temporary protection removal is not here, on purpose
	 * This function is meant to be used instead of clk_rate_protect,
	 * so before the consumer code path protect the clock provider
	 */

	ret = clk_core_set_rate_nolock(clk->core, rate);
	if (!ret) {
		clk_core_rate_protect(clk->core);
		clk->exclusive_count++;
	}

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate_exclusive);

/**
 * clk_set_rate_range - set a rate range for a clock source
 * @clk: clock source
 * @min: desired minimum clock rate in Hz, inclusive
 * @max: desired maximum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_rate_range(struct clk *clk, unsigned long min, unsigned long max)
{
	int ret = 0;
	unsigned long old_min, old_max, rate;

	if (!clk)
		return 0;

	trace_clk_set_rate_range(clk->core, min, max);

	if (min > max) {
		pr_err("%s: clk %s dev %s con %s: invalid range [%lu, %lu]\n",
		       __func__, clk->core->name, clk->dev_id, clk->con_id,
		       min, max);
		return -EINVAL;
	}

	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

	/* Save the current values in case we need to rollback the change */
	old_min = clk->min_rate;
	old_max = clk->max_rate;
	clk->min_rate = min;
	clk->max_rate = max;

	rate = clk_core_get_rate_nolock(clk->core);
	if (rate < min || rate > max) {
		/*
		 * FIXME:
		 * We are in bit of trouble here, current rate is outside the
		 * the requested range. We are going try to request appropriate
		 * range boundary but there is a catch. It may fail for the
		 * usual reason (clock broken, clock protected, etc) but also
		 * because:
		 * - round_rate() was not favorable and fell on the wrong
		 *   side of the boundary
		 * - the determine_rate() callback does not really check for
		 *   this corner case when determining the rate
		 */

		if (rate < min)
			rate = min;
		else
			rate = max;

		ret = clk_core_set_rate_nolock(clk->core, rate);
		if (ret) {
			/* rollback the changes */
			clk->min_rate = old_min;
			clk->max_rate = old_max;
		}
	}

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate_range);

/**
 * clk_set_min_rate - set a minimum clock rate for a clock source
 * @clk: clock source
 * @rate: desired minimum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_min_rate(struct clk *clk, unsigned long rate)
{
	if (!clk)
		return 0;

	trace_clk_set_min_rate(clk->core, rate);

	return clk_set_rate_range(clk, rate, clk->max_rate);
}
EXPORT_SYMBOL_GPL(clk_set_min_rate);

/**
 * clk_set_max_rate - set a maximum clock rate for a clock source
 * @clk: clock source
 * @rate: desired maximum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_max_rate(struct clk *clk, unsigned long rate)
{
	if (!clk)
		return 0;

	trace_clk_set_max_rate(clk->core, rate);

	return clk_set_rate_range(clk, clk->min_rate, rate);
}
EXPORT_SYMBOL_GPL(clk_set_max_rate);

/**
 * clk_get_parent - return the parent of a clk
 * @clk: the clk whose parent gets returned
 *
 * Simply returns clk->parent.  Returns NULL if clk is NULL.
 */
struct clk *clk_get_parent(struct clk *clk)
{
	struct clk *parent;

	if (!clk)
		return NULL;

	clk_prepare_lock();
	/* TODO: Create a per-user clk and change callers to call clk_put */
	parent = !clk->core->parent ? NULL : clk->core->parent->hw->clk;
	clk_prepare_unlock();

	return parent;
}
EXPORT_SYMBOL_GPL(clk_get_parent);

static struct clk_core *__clk_init_parent(struct clk_core *core)
{
	u8 index = 0;

/*
 * IAMROOT, 2022.08.06:
 * - num_parents가 0이면 안구해온다.
 */
	if (core->num_parents > 1 && core->ops->get_parent)
		index = core->ops->get_parent(core->hw);

	return clk_core_get_parent_by_index(core, index);
}

static void clk_core_reparent(struct clk_core *core,
				  struct clk_core *new_parent)
{
	clk_reparent(core, new_parent);
	__clk_recalc_accuracies(core);
	__clk_recalc_rates(core, POST_RATE_CHANGE);
}

void clk_hw_reparent(struct clk_hw *hw, struct clk_hw *new_parent)
{
	if (!hw)
		return;

	clk_core_reparent(hw->core, !new_parent ? NULL : new_parent->core);
}

/**
 * clk_has_parent - check if a clock is a possible parent for another
 * @clk: clock source
 * @parent: parent clock source
 *
 * This function can be used in drivers that need to check that a clock can be
 * the parent of another without actually changing the parent.
 *
 * Returns true if @parent is a possible parent for @clk, false otherwise.
 */
bool clk_has_parent(struct clk *clk, struct clk *parent)
{
	struct clk_core *core, *parent_core;
	int i;

	/* NULL clocks should be nops, so return success if either is NULL. */
	if (!clk || !parent)
		return true;

	core = clk->core;
	parent_core = parent->core;

	/* Optimize for the case where the parent is already the parent. */
	if (core->parent == parent_core)
		return true;

	for (i = 0; i < core->num_parents; i++)
		if (!strcmp(core->parents[i].name, parent_core->name))
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(clk_has_parent);

/*
 * IAMROOT, 2022.08.06:
 * - @core의 parent를 @parent로 지정한다.
 *   즉 mux clk의 입력소스로 @parent를 선택한다.
 */
static int clk_core_set_parent_nolock(struct clk_core *core,
				      struct clk_core *parent)
{
	int ret = 0;
	int p_index = 0;
	unsigned long p_rate = 0;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return 0;

	if (core->parent == parent)
		return 0;

	/* verify ops for multi-parent clks */
	if (core->num_parents > 1 && !core->ops->set_parent)
		return -EPERM;

	/* check that we are allowed to re-parent if the clock is in use */
	if ((core->flags & CLK_SET_PARENT_GATE) && core->prepare_count)
		return -EBUSY;

	if (clk_core_rate_is_protected(core))
		return -EBUSY;

/*
 * IAMROOT, 2022.08.06:
 * - core에 parent가 fetch되있는지 확인한다.
 */
	/* try finding the new parent index */
	if (parent) {
		p_index = clk_fetch_parent_index(core, parent);
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
					__func__, parent->name, core->name);
			return p_index;
		}
		p_rate = parent->rate;
	}

	ret = clk_pm_runtime_get(core);
	if (ret)
		return ret;

	/* propagate PRE_RATE_CHANGE notifications */
	ret = __clk_speculate_rates(core, p_rate);

	/* abort if a driver objects */
	if (ret & NOTIFY_STOP_MASK)
		goto runtime_put;

	/* do the re-parent */
	ret = __clk_set_parent(core, parent, p_index);

	/* propagate rate an accuracy recalculation accordingly */
	if (ret) {
		__clk_recalc_rates(core, ABORT_RATE_CHANGE);
	} else {
		__clk_recalc_rates(core, POST_RATE_CHANGE);
		__clk_recalc_accuracies(core);
	}

runtime_put:
	clk_pm_runtime_put(core);

	return ret;
}

int clk_hw_set_parent(struct clk_hw *hw, struct clk_hw *parent)
{
	return clk_core_set_parent_nolock(hw->core, parent->core);
}
EXPORT_SYMBOL_GPL(clk_hw_set_parent);

/**
 * clk_set_parent - switch the parent of a mux clk
 * @clk: the mux clk whose input we are switching
 * @parent: the new input to clk
 *
 * Re-parent clk to use parent as its new input source.  If clk is in
 * prepared state, the clk will get enabled for the duration of this call. If
 * that's not acceptable for a specific clk (Eg: the consumer can't handle
 * that, the reparenting is glitchy in hardware, etc), use the
 * CLK_SET_PARENT_GATE flag to allow reparenting only when clk is unprepared.
 *
 * After successfully changing clk's parent clk_set_parent will update the
 * clk topology, sysfs topology and propagate rate recalculation via
 * __clk_recalc_rates.
 *
 * Returns 0 on success, -EERROR otherwise.
 */

/*
 * IAMROOT, 2022.08.06:
 * - papgo
 *   부모를 새 입력 소스로 사용하려면 다시 부모로 지정합니다. clk가 준비 상태에
 *   있으면 이 호출 기간 동안 clk가 활성화됩니다. 특정 clk에 허용되지 않는 경우
 *   (예: 소비자가 이를 처리할 수 없고 하드웨어에서 부모 재지정이 결함이 있는 경우 등)
 *   CLK_SET_PARENT_GATE 플래그를 사용하여 clk가 준비되지 않은 경우에만 부모
 *   재지정을 허용합니다.
 *
 *   clk의 부모를 성공적으로 변경한 후 clk_set_parent는 clk 토폴로지, sysfs
 *   토폴로지를 업데이트하고 __clk_recalc_rates를 통해 속도 재계산을 전파합니다.
 *
 *   성공하면 0을 반환하고 그렇지 않으면 -EERROR를 반환합니다
 */
int clk_set_parent(struct clk *clk, struct clk *parent)
{
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

	ret = clk_core_set_parent_nolock(clk->core,
					 parent ? parent->core : NULL);

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_parent);

static int clk_core_set_phase_nolock(struct clk_core *core, int degrees)
{
	int ret = -EINVAL;

	lockdep_assert_held(&prepare_lock);

	if (!core)
		return 0;

	if (clk_core_rate_is_protected(core))
		return -EBUSY;

	trace_clk_set_phase(core, degrees);

	if (core->ops->set_phase) {
		ret = core->ops->set_phase(core->hw, degrees);
		if (!ret)
			core->phase = degrees;
	}

	trace_clk_set_phase_complete(core, degrees);

	return ret;
}

/**
 * clk_set_phase - adjust the phase shift of a clock signal
 * @clk: clock signal source
 * @degrees: number of degrees the signal is shifted
 *
 * Shifts the phase of a clock signal by the specified
 * degrees. Returns 0 on success, -EERROR otherwise.
 *
 * This function makes no distinction about the input or reference
 * signal that we adjust the clock signal phase against. For example
 * phase locked-loop clock signal generators we may shift phase with
 * respect to feedback clock signal input, but for other cases the
 * clock phase may be shifted with respect to some other, unspecified
 * signal.
 *
 * Additionally the concept of phase shift does not propagate through
 * the clock tree hierarchy, which sets it apart from clock rates and
 * clock accuracy. A parent clock phase attribute does not have an
 * impact on the phase attribute of a child clock.
 */
int clk_set_phase(struct clk *clk, int degrees)
{
	int ret;

	if (!clk)
		return 0;

	/* sanity check degrees */
	degrees %= 360;
	if (degrees < 0)
		degrees += 360;

	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

	ret = clk_core_set_phase_nolock(clk->core, degrees);

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_phase);

/*
 * IAMROOT, 2022.08.06:
 * - get_phase ops가 있다면 함수를 수행해 위상을 알아온다.
 */
static int clk_core_get_phase(struct clk_core *core)
{
	int ret;

	lockdep_assert_held(&prepare_lock);
	if (!core->ops->get_phase)
		return 0;

	/* Always try to update cached phase if possible */
	ret = core->ops->get_phase(core->hw);
	if (ret >= 0)
		core->phase = ret;

	return ret;
}

/**
 * clk_get_phase - return the phase shift of a clock signal
 * @clk: clock signal source
 *
 * Returns the phase shift of a clock node in degrees, otherwise returns
 * -EERROR.
 */
int clk_get_phase(struct clk *clk)
{
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();
	ret = clk_core_get_phase(clk->core);
	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_get_phase);

/*
 * IAMROOT, 2022.08.06:
 * - 50%를 default로 설정한다.
 */
static void clk_core_reset_duty_cycle_nolock(struct clk_core *core)
{
	/* Assume a default value of 50% */
	core->duty.num = 1;
	core->duty.den = 2;
}

static int clk_core_update_duty_cycle_parent_nolock(struct clk_core *core);

/*
 * IAMROOT, 2022.08.06:
 * - parent가 있고 parent것을 사용하라고하면 parent로 타고 올라가고, 아니면
 *   자기것을 사용한다.
 * - 아무대도 없을 경우 default로 50%를 사용한다.
 */
static int clk_core_update_duty_cycle_nolock(struct clk_core *core)
{
	struct clk_duty *duty = &core->duty;
	int ret = 0;

/*
 * IAMROOT, 2022.08.06:
 * - ops가 없으면 parent에서 
 */
	if (!core->ops->get_duty_cycle)
		return clk_core_update_duty_cycle_parent_nolock(core);

	ret = core->ops->get_duty_cycle(core->hw, duty);
	if (ret)
		goto reset;

	/* Don't trust the clock provider too much */
	if (duty->den == 0 || duty->num > duty->den) {
		ret = -EINVAL;
		goto reset;
	}

	return 0;

reset:
	clk_core_reset_duty_cycle_nolock(core);
	return ret;
}

/*
 * IAMROOT, 2022.08.06:
 * - parent가 존재하고 flag가 parent꺼를 사용하라고 되있으면 parent사용한다.
 *   아니라면 default로 50%를 사용한다.
 * - 재귀함수로 되있다.
 */
static int clk_core_update_duty_cycle_parent_nolock(struct clk_core *core)
{
	int ret = 0;

	if (core->parent &&
	    core->flags & CLK_DUTY_CYCLE_PARENT) {
		ret = clk_core_update_duty_cycle_nolock(core->parent);
		memcpy(&core->duty, &core->parent->duty, sizeof(core->duty));
	} else {
		clk_core_reset_duty_cycle_nolock(core);
	}

	return ret;
}

static int clk_core_set_duty_cycle_parent_nolock(struct clk_core *core,
						 struct clk_duty *duty);

static int clk_core_set_duty_cycle_nolock(struct clk_core *core,
					  struct clk_duty *duty)
{
	int ret;

	lockdep_assert_held(&prepare_lock);

	if (clk_core_rate_is_protected(core))
		return -EBUSY;

	trace_clk_set_duty_cycle(core, duty);

	if (!core->ops->set_duty_cycle)
		return clk_core_set_duty_cycle_parent_nolock(core, duty);

	ret = core->ops->set_duty_cycle(core->hw, duty);
	if (!ret)
		memcpy(&core->duty, duty, sizeof(*duty));

	trace_clk_set_duty_cycle_complete(core, duty);

	return ret;
}

static int clk_core_set_duty_cycle_parent_nolock(struct clk_core *core,
						 struct clk_duty *duty)
{
	int ret = 0;

	if (core->parent &&
	    core->flags & (CLK_DUTY_CYCLE_PARENT | CLK_SET_RATE_PARENT)) {
		ret = clk_core_set_duty_cycle_nolock(core->parent, duty);
		memcpy(&core->duty, &core->parent->duty, sizeof(core->duty));
	}

	return ret;
}

/**
 * clk_set_duty_cycle - adjust the duty cycle ratio of a clock signal
 * @clk: clock signal source
 * @num: numerator of the duty cycle ratio to be applied
 * @den: denominator of the duty cycle ratio to be applied
 *
 * Apply the duty cycle ratio if the ratio is valid and the clock can
 * perform this operation
 *
 * Returns (0) on success, a negative errno otherwise.
 */
int clk_set_duty_cycle(struct clk *clk, unsigned int num, unsigned int den)
{
	int ret;
	struct clk_duty duty;

	if (!clk)
		return 0;

	/* sanity check the ratio */
	if (den == 0 || num > den)
		return -EINVAL;

	duty.num = num;
	duty.den = den;

	clk_prepare_lock();

	if (clk->exclusive_count)
		clk_core_rate_unprotect(clk->core);

	ret = clk_core_set_duty_cycle_nolock(clk->core, &duty);

	if (clk->exclusive_count)
		clk_core_rate_protect(clk->core);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_duty_cycle);

static int clk_core_get_scaled_duty_cycle(struct clk_core *core,
					  unsigned int scale)
{
	struct clk_duty *duty = &core->duty;
	int ret;

	clk_prepare_lock();

	ret = clk_core_update_duty_cycle_nolock(core);
	if (!ret)
		ret = mult_frac(scale, duty->num, duty->den);

	clk_prepare_unlock();

	return ret;
}

/**
 * clk_get_scaled_duty_cycle - return the duty cycle ratio of a clock signal
 * @clk: clock signal source
 * @scale: scaling factor to be applied to represent the ratio as an integer
 *
 * Returns the duty cycle ratio of a clock node multiplied by the provided
 * scaling factor, or negative errno on error.
 */
int clk_get_scaled_duty_cycle(struct clk *clk, unsigned int scale)
{
	if (!clk)
		return 0;

	return clk_core_get_scaled_duty_cycle(clk->core, scale);
}
EXPORT_SYMBOL_GPL(clk_get_scaled_duty_cycle);

/**
 * clk_is_match - check if two clk's point to the same hardware clock
 * @p: clk compared against q
 * @q: clk compared against p
 *
 * Returns true if the two struct clk pointers both point to the same hardware
 * clock node. Put differently, returns true if struct clk *p and struct clk *q
 * share the same struct clk_core object.
 *
 * Returns false otherwise. Note that two NULL clks are treated as matching.
 */
bool clk_is_match(const struct clk *p, const struct clk *q)
{
	/* trivial case: identical struct clk's or both NULL */
	if (p == q)
		return true;

	/* true if clk->core pointers match. Avoid dereferencing garbage */
	if (!IS_ERR_OR_NULL(p) && !IS_ERR_OR_NULL(q))
		if (p->core == q->core)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(clk_is_match);

/***        debugfs support        ***/

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *rootdir;
static int inited = 0;
static DEFINE_MUTEX(clk_debug_lock);
static HLIST_HEAD(clk_debug_list);

static struct hlist_head *orphan_list[] = {
	&clk_orphan_list,
	NULL,
};

static void clk_summary_show_one(struct seq_file *s, struct clk_core *c,
				 int level)
{
	int phase;

	seq_printf(s, "%*s%-*s %7d %8d %8d %11lu %10lu ",
		   level * 3 + 1, "",
		   30 - level * 3, c->name,
		   c->enable_count, c->prepare_count, c->protect_count,
		   clk_core_get_rate_recalc(c),
		   clk_core_get_accuracy_recalc(c));

	phase = clk_core_get_phase(c);
	if (phase >= 0)
		seq_printf(s, "%5d", phase);
	else
		seq_puts(s, "-----");

	seq_printf(s, " %6d", clk_core_get_scaled_duty_cycle(c, 100000));

	if (c->ops->is_enabled)
		seq_printf(s, " %9c\n", clk_core_is_enabled(c) ? 'Y' : 'N');
	else if (!c->ops->enable)
		seq_printf(s, " %9c\n", 'Y');
	else
		seq_printf(s, " %9c\n", '?');
}

static void clk_summary_show_subtree(struct seq_file *s, struct clk_core *c,
				     int level)
{
	struct clk_core *child;

	clk_summary_show_one(s, c, level);

	hlist_for_each_entry(child, &c->children, child_node)
		clk_summary_show_subtree(s, child, level + 1);
}

static int clk_summary_show(struct seq_file *s, void *data)
{
	struct clk_core *c;
	struct hlist_head **lists = (struct hlist_head **)s->private;

	seq_puts(s, "                                 enable  prepare  protect                                duty  hardware\n");
	seq_puts(s, "   clock                          count    count    count        rate   accuracy phase  cycle    enable\n");
	seq_puts(s, "-------------------------------------------------------------------------------------------------------\n");

	clk_prepare_lock();

	for (; *lists; lists++)
		hlist_for_each_entry(c, *lists, child_node)
			clk_summary_show_subtree(s, c, 0);

	clk_prepare_unlock();

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_summary);

static void clk_dump_one(struct seq_file *s, struct clk_core *c, int level)
{
	int phase;
	unsigned long min_rate, max_rate;

	clk_core_get_boundaries(c, &min_rate, &max_rate);

	/* This should be JSON format, i.e. elements separated with a comma */
	seq_printf(s, "\"%s\": { ", c->name);
	seq_printf(s, "\"enable_count\": %d,", c->enable_count);
	seq_printf(s, "\"prepare_count\": %d,", c->prepare_count);
	seq_printf(s, "\"protect_count\": %d,", c->protect_count);
	seq_printf(s, "\"rate\": %lu,", clk_core_get_rate_recalc(c));
	seq_printf(s, "\"min_rate\": %lu,", min_rate);
	seq_printf(s, "\"max_rate\": %lu,", max_rate);
	seq_printf(s, "\"accuracy\": %lu,", clk_core_get_accuracy_recalc(c));
	phase = clk_core_get_phase(c);
	if (phase >= 0)
		seq_printf(s, "\"phase\": %d,", phase);
	seq_printf(s, "\"duty_cycle\": %u",
		   clk_core_get_scaled_duty_cycle(c, 100000));
}

static void clk_dump_subtree(struct seq_file *s, struct clk_core *c, int level)
{
	struct clk_core *child;

	clk_dump_one(s, c, level);

	hlist_for_each_entry(child, &c->children, child_node) {
		seq_putc(s, ',');
		clk_dump_subtree(s, child, level + 1);
	}

	seq_putc(s, '}');
}

static int clk_dump_show(struct seq_file *s, void *data)
{
	struct clk_core *c;
	bool first_node = true;
	struct hlist_head **lists = (struct hlist_head **)s->private;

	seq_putc(s, '{');
	clk_prepare_lock();

	for (; *lists; lists++) {
		hlist_for_each_entry(c, *lists, child_node) {
			if (!first_node)
				seq_putc(s, ',');
			first_node = false;
			clk_dump_subtree(s, c, 0);
		}
	}

	clk_prepare_unlock();

	seq_puts(s, "}\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_dump);

#undef CLOCK_ALLOW_WRITE_DEBUGFS
#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
/*
 * This can be dangerous, therefore don't provide any real compile time
 * configuration option for this feature.
 * People who want to use this will need to modify the source code directly.
 */
static int clk_rate_set(void *data, u64 val)
{
	struct clk_core *core = data;
	int ret;

	clk_prepare_lock();
	ret = clk_core_set_rate_nolock(core, val);
	clk_prepare_unlock();

	return ret;
}

#define clk_rate_mode	0644

static int clk_prepare_enable_set(void *data, u64 val)
{
	struct clk_core *core = data;
	int ret = 0;

	if (val)
		ret = clk_prepare_enable(core->hw->clk);
	else
		clk_disable_unprepare(core->hw->clk);

	return ret;
}

static int clk_prepare_enable_get(void *data, u64 *val)
{
	struct clk_core *core = data;

	*val = core->enable_count && core->prepare_count;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clk_prepare_enable_fops, clk_prepare_enable_get,
			 clk_prepare_enable_set, "%llu\n");

#else
#define clk_rate_set	NULL
#define clk_rate_mode	0444
#endif

static int clk_rate_get(void *data, u64 *val)
{
	struct clk_core *core = data;

	*val = core->rate;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clk_rate_fops, clk_rate_get, clk_rate_set, "%llu\n");

static const struct {
	unsigned long flag;
	const char *name;
} clk_flags[] = {
#define ENTRY(f) { f, #f }
	ENTRY(CLK_SET_RATE_GATE),
	ENTRY(CLK_SET_PARENT_GATE),
	ENTRY(CLK_SET_RATE_PARENT),
	ENTRY(CLK_IGNORE_UNUSED),
	ENTRY(CLK_GET_RATE_NOCACHE),
	ENTRY(CLK_SET_RATE_NO_REPARENT),
	ENTRY(CLK_GET_ACCURACY_NOCACHE),
	ENTRY(CLK_RECALC_NEW_RATES),
	ENTRY(CLK_SET_RATE_UNGATE),
	ENTRY(CLK_IS_CRITICAL),
	ENTRY(CLK_OPS_PARENT_ENABLE),
	ENTRY(CLK_DUTY_CYCLE_PARENT),
#undef ENTRY
};

static int clk_flags_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;
	unsigned long flags = core->flags;
	unsigned int i;

	for (i = 0; flags && i < ARRAY_SIZE(clk_flags); i++) {
		if (flags & clk_flags[i].flag) {
			seq_printf(s, "%s\n", clk_flags[i].name);
			flags &= ~clk_flags[i].flag;
		}
	}
	if (flags) {
		/* Unknown flags */
		seq_printf(s, "0x%lx\n", flags);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_flags);

static void possible_parent_show(struct seq_file *s, struct clk_core *core,
				 unsigned int i, char terminator)
{
	struct clk_core *parent;

	/*
	 * Go through the following options to fetch a parent's name.
	 *
	 * 1. Fetch the registered parent clock and use its name
	 * 2. Use the global (fallback) name if specified
	 * 3. Use the local fw_name if provided
	 * 4. Fetch parent clock's clock-output-name if DT index was set
	 *
	 * This may still fail in some cases, such as when the parent is
	 * specified directly via a struct clk_hw pointer, but it isn't
	 * registered (yet).
	 */
	parent = clk_core_get_parent_by_index(core, i);
	if (parent)
		seq_puts(s, parent->name);
	else if (core->parents[i].name)
		seq_puts(s, core->parents[i].name);
	else if (core->parents[i].fw_name)
		seq_printf(s, "<%s>(fw)", core->parents[i].fw_name);
	else if (core->parents[i].index >= 0)
		seq_puts(s,
			 of_clk_get_parent_name(core->of_node,
						core->parents[i].index));
	else
		seq_puts(s, "(missing)");

	seq_putc(s, terminator);
}

static int possible_parents_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;
	int i;

	for (i = 0; i < core->num_parents - 1; i++)
		possible_parent_show(s, core, i, ' ');

	possible_parent_show(s, core, i, '\n');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(possible_parents);

static int current_parent_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;

	if (core->parent)
		seq_printf(s, "%s\n", core->parent->name);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(current_parent);

static int clk_duty_cycle_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;
	struct clk_duty *duty = &core->duty;

	seq_printf(s, "%u/%u\n", duty->num, duty->den);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_duty_cycle);

static int clk_min_rate_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;
	unsigned long min_rate, max_rate;

	clk_prepare_lock();
	clk_core_get_boundaries(core, &min_rate, &max_rate);
	clk_prepare_unlock();
	seq_printf(s, "%lu\n", min_rate);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_min_rate);

static int clk_max_rate_show(struct seq_file *s, void *data)
{
	struct clk_core *core = s->private;
	unsigned long min_rate, max_rate;

	clk_prepare_lock();
	clk_core_get_boundaries(core, &min_rate, &max_rate);
	clk_prepare_unlock();
	seq_printf(s, "%lu\n", max_rate);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_max_rate);

static void clk_debug_create_one(struct clk_core *core, struct dentry *pdentry)
{
	struct dentry *root;

	if (!core || !pdentry)
		return;

	root = debugfs_create_dir(core->name, pdentry);
	core->dentry = root;

	debugfs_create_file("clk_rate", clk_rate_mode, root, core,
			    &clk_rate_fops);
	debugfs_create_file("clk_min_rate", 0444, root, core, &clk_min_rate_fops);
	debugfs_create_file("clk_max_rate", 0444, root, core, &clk_max_rate_fops);
	debugfs_create_ulong("clk_accuracy", 0444, root, &core->accuracy);
	debugfs_create_u32("clk_phase", 0444, root, &core->phase);
	debugfs_create_file("clk_flags", 0444, root, core, &clk_flags_fops);
	debugfs_create_u32("clk_prepare_count", 0444, root, &core->prepare_count);
	debugfs_create_u32("clk_enable_count", 0444, root, &core->enable_count);
	debugfs_create_u32("clk_protect_count", 0444, root, &core->protect_count);
	debugfs_create_u32("clk_notifier_count", 0444, root, &core->notifier_count);
	debugfs_create_file("clk_duty_cycle", 0444, root, core,
			    &clk_duty_cycle_fops);
#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
	debugfs_create_file("clk_prepare_enable", 0644, root, core,
			    &clk_prepare_enable_fops);
#endif

	if (core->num_parents > 0)
		debugfs_create_file("clk_parent", 0444, root, core,
				    &current_parent_fops);

	if (core->num_parents > 1)
		debugfs_create_file("clk_possible_parents", 0444, root, core,
				    &possible_parents_fops);

	if (core->ops->debug_init)
		core->ops->debug_init(core->hw, core->dentry);
}

/**
 * clk_debug_register - add a clk node to the debugfs clk directory
 * @core: the clk being added to the debugfs clk directory
 *
 * Dynamically adds a clk to the debugfs clk directory if debugfs has been
 * initialized.  Otherwise it bails out early since the debugfs clk directory
 * will be created lazily by clk_debug_init as part of a late_initcall.
 */
static void clk_debug_register(struct clk_core *core)
{
	mutex_lock(&clk_debug_lock);
	hlist_add_head(&core->debug_node, &clk_debug_list);
	if (inited)
		clk_debug_create_one(core, rootdir);
	mutex_unlock(&clk_debug_lock);
}

 /**
 * clk_debug_unregister - remove a clk node from the debugfs clk directory
 * @core: the clk being removed from the debugfs clk directory
 *
 * Dynamically removes a clk and all its child nodes from the
 * debugfs clk directory if clk->dentry points to debugfs created by
 * clk_debug_register in __clk_core_init.
 */
static void clk_debug_unregister(struct clk_core *core)
{
	mutex_lock(&clk_debug_lock);
	hlist_del_init(&core->debug_node);
	debugfs_remove_recursive(core->dentry);
	core->dentry = NULL;
	mutex_unlock(&clk_debug_lock);
}

/**
 * clk_debug_init - lazily populate the debugfs clk directory
 *
 * clks are often initialized very early during boot before memory can be
 * dynamically allocated and well before debugfs is setup. This function
 * populates the debugfs clk directory once at boot-time when we know that
 * debugfs is setup. It should only be called once at boot-time, all other clks
 * added dynamically will be done so with clk_debug_register.
 */
static int __init clk_debug_init(void)
{
	struct clk_core *core;

	rootdir = debugfs_create_dir("clk", NULL);

	debugfs_create_file("clk_summary", 0444, rootdir, &all_lists,
			    &clk_summary_fops);
	debugfs_create_file("clk_dump", 0444, rootdir, &all_lists,
			    &clk_dump_fops);
	debugfs_create_file("clk_orphan_summary", 0444, rootdir, &orphan_list,
			    &clk_summary_fops);
	debugfs_create_file("clk_orphan_dump", 0444, rootdir, &orphan_list,
			    &clk_dump_fops);

	mutex_lock(&clk_debug_lock);
	hlist_for_each_entry(core, &clk_debug_list, debug_node)
		clk_debug_create_one(core, rootdir);

	inited = 1;
	mutex_unlock(&clk_debug_lock);

	return 0;
}
late_initcall(clk_debug_init);
#else
static inline void clk_debug_register(struct clk_core *core) { }
static inline void clk_debug_unregister(struct clk_core *core)
{
}
#endif

/*
 * IAMROOT, 2022.08.06:
 * - clk_orphan_list를 순회하며 orphans이 였던 것들중에 parent가 생긴것이
 *   있는 확인하여 작업을 해준다.
 */
static void clk_core_reparent_orphans_nolock(void)
{
	struct clk_core *orphan;
	struct hlist_node *tmp2;

	/*
	 * walk the list of orphan clocks and reparent any that newly finds a
	 * parent.
	 */
	hlist_for_each_entry_safe(orphan, tmp2, &clk_orphan_list, child_node) {
		struct clk_core *parent = __clk_init_parent(orphan);

		/*
		 * We need to use __clk_set_parent_before() and _after() to
		 * to properly migrate any prepare/enable count of the orphan
		 * clock. This is important for CLK_IS_CRITICAL clocks, which
		 * are enabled during init but might not have a parent yet.
		 */
		if (parent) {
			/* update the clk tree topology */
			__clk_set_parent_before(orphan, parent);
			__clk_set_parent_after(orphan, parent, NULL);
			__clk_recalc_accuracies(orphan);
			__clk_recalc_rates(orphan, 0);
		}
	}
}

/**
 * __clk_core_init - initialize the data structures in a struct clk_core
 * @core:	clk_core being initialized
 *
 * Initializes the lists in struct clk_core, queries the hardware for the
 * parent and rate and sets them both.
 */
/*
 * IAMROOT, 2022.08.13:
 * - 1. ops 검사.
 *   2. parent가 없는 @core면 root.
 *      parent가 있는데 parent가 아직 등록안됬으면 orphan.
 *      parent가 있는데 parent가 있으면 parent에 등록.
 */
static int __clk_core_init(struct clk_core *core)
{
	int ret;
	struct clk_core *parent;
	unsigned long rate;
	int phase;

	if (!core)
		return -EINVAL;

/*
 * IAMROOT, 2022.08.13:
 * - lock.
 */
	clk_prepare_lock();

/*
 * IAMROOT, 2022.08.13:
 * - 절전모드였을 경우 다시 power on
 */
	ret = clk_pm_runtime_get(core);
	if (ret)
		goto unlock;

/*
 * IAMROOT, 2022.08.13:
 * - core를 찾는다.
 */
	/* check to see if a clock with this name is already registered */
	if (clk_core_lookup(core->name)) {
		pr_debug("%s: clk %s already initialized\n",
				__func__, core->name);
		ret = -EEXIST;
		goto out;
	}

/*
 * IAMROOT, 2022.08.13:
 * - ops를 전체적으로 잘 설정되있는지 검사한다.
 */
	/* check that clk_ops are sane.  See Documentation/driver-api/clk.rst */
/*
 * IAMROOT, 2022.08.13:
 * - set_rate가 지정되면 round_rate, determine_rate 둘중하나와
 *   recalc_rate가 존재해야된다.
 *   설정을 못하는 clk이 왔을때 대처방안을 마련해줘야되는 개념.
 * ex) clk_factor_round_rate, clk_factor_set_rate, clk_factor_recalc_rate,
 */
	if (core->ops->set_rate &&
	    !((core->ops->round_rate || core->ops->determine_rate) &&
	      core->ops->recalc_rate)) {
		pr_err("%s: %s must implement .round_rate or .determine_rate in addition to .recalc_rate\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->ops->set_parent && !core->ops->get_parent) {
		pr_err("%s: %s must implement .get_parent & .set_parent\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->num_parents > 1 && !core->ops->get_parent) {
		pr_err("%s: %s must implement .get_parent as it has multi parents\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->ops->set_rate_and_parent &&
			!(core->ops->set_parent && core->ops->set_rate)) {
		pr_err("%s: %s must implement .set_parent & .set_rate\n",
				__func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * optional platform-specific magic
	 *
	 * The .init callback is not used by any of the basic clock types, but
	 * exists for weird hardware that must perform initialization magic for
	 * CCF to get an accurate view of clock for any other callbacks. It may
	 * also be used needs to perform dynamic allocations. Such allocation
	 * must be freed in the terminate() callback.
	 * This callback shall not be used to initialize the parameters state,
	 * such as rate, parent, etc ...
	 *
	 * If it exist, this callback should called before any other callback of
	 * the clock
	 */
	if (core->ops->init) {
		ret = core->ops->init(core->hw);
		if (ret)
			goto out;
	}

/*
 * IAMROOT, 2022.08.06:
 * - num_parents를 사용하여 parent가 있는지 판단해본다.
 *   clk_hw_register_fixed_rate_with_accuracy()를 통해서 이 루틴 까지 왔을때
 *   clk_hw_register_fixed_rate_with_accuracy()에서 parent 관련 인자들이
 *   NULL값이였으면 num_parents가 0이 되어 parent를 안구한다.
 */
	parent = core->parent = __clk_init_parent(core);

	/*
	 * Populate core->parent if parent has already been clk_core_init'd. If
	 * parent has not yet been clk_core_init'd then place clk in the orphan
	 * list.  If clk doesn't have any parents then place it in the root
	 * clk list.
	 *
	 * Every time a new clk is clk_init'd then we walk the list of orphan
	 * clocks and re-parent any that are children of the clock currently
	 * being clk_init'd.
	 */
/*
 * IAMROOT, 2022.08.06:
 * - parent가 없는데, num_parents가 0이라는것은 root라는 것이고 num_parents가
 *   존재하면 일단 orphan상태로 한다.
 */
	if (parent) {
		hlist_add_head(&core->child_node, &parent->children);
		core->orphan = parent->orphan;
	} else if (!core->num_parents) {
/*
 * IAMROOT, 2022.08.13:
 * - 최상위 fixed factor면 root로 등록한다.
 */
		hlist_add_head(&core->child_node, &clk_root_list);
		core->orphan = false;
	} else {
/*
 * IAMROOT, 2022.08.13:
 * - parent가 있는데 . 못찾은경우.(아직 parent가 등록안되있음.). orphan.
 */
		hlist_add_head(&core->child_node, &clk_orphan_list);
		core->orphan = true;
	}

	/*
	 * Set clk's accuracy.  The preferred method is to use
	 * .recalc_accuracy. For simple clocks and lazy developers the default
	 * fallback is to use the parent's accuracy.  If a clock doesn't have a
	 * parent (or is orphaned) then accuracy is set to zero (perfect
	 * clock).
	 */
/*
 * IAMROOT, 2022.08.06:
 * - ex) clk_fixed_rate_recalc_accuracy,
 */
	if (core->ops->recalc_accuracy)
		core->accuracy = core->ops->recalc_accuracy(core->hw,
					clk_core_get_accuracy_no_lock(parent));
	else if (parent)
		core->accuracy = parent->accuracy;
	else
		core->accuracy = 0;

	/*
	 * Set clk's phase by clk_core_get_phase() caching the phase.
	 * Since a phase is by definition relative to its parent, just
	 * query the current clock phase, or just assume it's in phase.
	 */
	phase = clk_core_get_phase(core);
	if (phase < 0) {
		ret = phase;
		pr_warn("%s: Failed to get phase for clk '%s'\n", __func__,
			core->name);
		goto out;
	}

	/*
	 * Set clk's duty cycle.
	 */
	clk_core_update_duty_cycle_nolock(core);

	/*
	 * Set clk's rate.  The preferred method is to use .recalc_rate.  For
	 * simple clocks and lazy developers the default fallback is to use the
	 * parent's rate.  If a clock doesn't have a parent (or is orphaned)
	 * then rate is set to zero.
	 */
	if (core->ops->recalc_rate)
		rate = core->ops->recalc_rate(core->hw,
				clk_core_get_rate_nolock(parent));
	else if (parent)
		rate = parent->rate;
	else
		rate = 0;
	core->rate = core->req_rate = rate;

	/*
	 * Enable CLK_IS_CRITICAL clocks so newly added critical clocks
	 * don't get accidentally disabled when walking the orphan tree and
	 * reparenting clocks
	 */
	if (core->flags & CLK_IS_CRITICAL) {
		ret = clk_core_prepare(core);
		if (ret) {
			pr_warn("%s: critical clk '%s' failed to prepare\n",
			       __func__, core->name);
			goto out;
		}

		ret = clk_core_enable_lock(core);
		if (ret) {
			pr_warn("%s: critical clk '%s' failed to enable\n",
			       __func__, core->name);
			clk_core_unprepare(core);
			goto out;
		}
	}

	clk_core_reparent_orphans_nolock();


	kref_init(&core->ref);
out:
	clk_pm_runtime_put(core);
unlock:
	if (ret)
		hlist_del_init(&core->child_node);

	clk_prepare_unlock();

	if (!ret)
		clk_debug_register(core);

	return ret;
}

/**
 * clk_core_link_consumer - Add a clk consumer to the list of consumers in a clk_core
 * @core: clk to add consumer to
 * @clk: consumer to link to a clk
 */
/*
 * IAMROOT, 2022.08.06:
 * - core(provider)에 clk(consumer) 등록.
 */
static void clk_core_link_consumer(struct clk_core *core, struct clk *clk)
{
	clk_prepare_lock();
	hlist_add_head(&clk->clks_node, &core->clks);
	clk_prepare_unlock();
}

/**
 * clk_core_unlink_consumer - Remove a clk consumer from the list of consumers in a clk_core
 * @clk: consumer to unlink
 */
static void clk_core_unlink_consumer(struct clk *clk)
{
	lockdep_assert_held(&prepare_lock);
	hlist_del(&clk->clks_node);
}

/**
 * alloc_clk - Allocate a clk consumer, but leave it unlinked to the clk_core
 * @core: clk to allocate a consumer for
 * @dev_id: string describing device name
 * @con_id: connection ID string on device
 *
 * Returns: clk consumer left unlinked from the consumer list
 */
/*
 * IAMROOT, 2022.08.06:
 * - struct clk 할당 및 초기화.
 */
static struct clk *alloc_clk(struct clk_core *core, const char *dev_id,
			     const char *con_id)
{
	struct clk *clk;

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->core = core;
	clk->dev_id = dev_id;
	clk->con_id = kstrdup_const(con_id, GFP_KERNEL);
	clk->max_rate = ULONG_MAX;

	return clk;
}

/**
 * free_clk - Free a clk consumer
 * @clk: clk consumer to free
 *
 * Note, this assumes the clk has been unlinked from the clk_core consumer
 * list.
 */
static void free_clk(struct clk *clk)
{
	kfree_const(clk->con_id);
	kfree(clk);
}

/**
 * clk_hw_create_clk: Allocate and link a clk consumer to a clk_core given
 * a clk_hw
 * @dev: clk consumer device
 * @hw: clk_hw associated with the clk being consumed
 * @dev_id: string describing device name
 * @con_id: connection ID string on device
 *
 * This is the main function used to create a clk pointer for use by clk
 * consumers. It connects a consumer to the clk_core and clk_hw structures
 * used by the framework and clk provider respectively.
 */
/*
 * IAMROOT, 2022.08.06:
 * - @hw가 error 라면 error return.
 * - clk core에 clk을 할당하여 추가한다.
 */
struct clk *clk_hw_create_clk(struct device *dev, struct clk_hw *hw,
			      const char *dev_id, const char *con_id)
{
	struct clk *clk;
	struct clk_core *core;

/*
 * IAMROOT, 2022.08.06:
 * - @hw가 error라면 error 변환 return.
 */
	/* This is to allow this function to be chained to others */
	if (IS_ERR_OR_NULL(hw))
		return ERR_CAST(hw);

	core = hw->core;
	clk = alloc_clk(core, dev_id, con_id);
	if (IS_ERR(clk))
		return clk;
	clk->dev = dev;

	if (!try_module_get(core->owner)) {
		free_clk(clk);
		return ERR_PTR(-ENOENT);
	}

/*
 * IAMROOT, 2022.08.06:
 * - clk core ref up
 */
	kref_get(&core->ref);

/*
 * IAMROOT, 2022.08.06:
 * - core(provider)에 clk(consumer)를 등록한다.
 */
	clk_core_link_consumer(core, clk);

	return clk;
}

/**
 * clk_hw_get_clk - get clk consumer given an clk_hw
 * @hw: clk_hw associated with the clk being consumed
 * @con_id: connection ID string on device
 *
 * Returns: new clk consumer
 * This is the function to be used by providers which need
 * to get a consumer clk and act on the clock element
 * Calls to this function must be balanced with calls clk_put()
 */
struct clk *clk_hw_get_clk(struct clk_hw *hw, const char *con_id)
{
	struct device *dev = hw->core->dev;

	return clk_hw_create_clk(dev, hw, dev_name(dev), con_id);
}
EXPORT_SYMBOL(clk_hw_get_clk);

static int clk_cpy_name(const char **dst_p, const char *src, bool must_exist)
{
	const char *dst;

	if (!src) {
		if (must_exist)
			return -EINVAL;
		return 0;
	}

	*dst_p = dst = kstrdup_const(src, GFP_KERNEL);
	if (!dst)
		return -ENOMEM;

	return 0;
}

/*
 * IAMROOT, 2022.08.13:
 * - parent_xxx 인자에 따라 core의 parent를 설정한다.
 *   각각의 parent index는 -1로 설정해놓는다. 아직 연결이 안되있다는 의미.
 */
static int clk_core_populate_parent_map(struct clk_core *core,
					const struct clk_init_data *init)
{
	u8 num_parents = init->num_parents;
	const char * const *parent_names = init->parent_names;
	const struct clk_hw **parent_hws = init->parent_hws;
	const struct clk_parent_data *parent_data = init->parent_data;
	int i, ret = 0;
	struct clk_parent_map *parents, *parent;

/*
 * IAMROOT, 2022.08.13:
 * - parent가 없으면 빠져나간다.
 */
	if (!num_parents)
		return 0;

/*
 * IAMROOT, 2022.08.13:
 * - parent가 있을 경우.
 */
	/*
	 * Avoid unnecessary string look-ups of clk_core's possible parents by
	 * having a cache of names/clk_hw pointers to clk_core pointers.
	 */
	parents = kcalloc(num_parents, sizeof(*parents), GFP_KERNEL);
	core->parents = parents;
	if (!parents)
		return -ENOMEM;

	/* Copy everything over because it might be __initdata */
	for (i = 0, parent = parents; i < num_parents; i++, parent++) {
		parent->index = -1;

/*
 * IAMROOT, 2022.08.13:
 * - 상위 함수에 따라 parent_name, parent_data, parent_hw 3가지중 한가지가
 *   무조건 있을 것이다.
 * - parent_name
 *   name만 copy한다.
 * - parent_data
 *   hw, index, name(fw_name으로 copy)을 설정한다.
 * - parent_hw
 *   hw만 설정한다.
 */
		if (parent_names) {
			/* throw a WARN if any entries are NULL */
			WARN(!parent_names[i],
				"%s: invalid NULL in %s's .parent_names\n",
				__func__, core->name);
			ret = clk_cpy_name(&parent->name, parent_names[i],
					   true);
		} else if (parent_data) {
			parent->hw = parent_data[i].hw;
			parent->index = parent_data[i].index;
			ret = clk_cpy_name(&parent->fw_name,
					   parent_data[i].fw_name, false);
			if (!ret)
				ret = clk_cpy_name(&parent->name,
						   parent_data[i].name,
						   false);
		} else if (parent_hws) {
			parent->hw = parent_hws[i];
		} else {
			ret = -EINVAL;
			WARN(1, "Must specify parents if num_parents > 0\n");
		}

		if (ret) {
			do {
				kfree_const(parents[i].name);
				kfree_const(parents[i].fw_name);
			} while (--i >= 0);
			kfree(parents);

			return ret;
		}
	}

	return 0;
}

static void clk_core_free_parent_map(struct clk_core *core)
{
	int i = core->num_parents;

	if (!core->num_parents)
		return;

	while (--i >= 0) {
		kfree_const(core->parents[i].name);
		kfree_const(core->parents[i].fw_name);
	}

	kfree(core->parents);
}

/*
 * IAMROOT, 2022.08.06:
 * - @hw로 받은 data로 core와 clk를 생성하고,
 *   root list or orphan list, 부모의 child 에 등록한다.
 */
static struct clk *
__clk_register(struct device *dev, struct device_node *np, struct clk_hw *hw)
{
	int ret;
	struct clk_core *core;
	const struct clk_init_data *init = hw->init;

	/*
	 * The init data is not supposed to be used outside of registration path.
	 * Set it to NULL so that provider drivers can't use it either and so that
	 * we catch use of hw->init early on in the core.
	 */
	hw->init = NULL;

	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (!core) {
		ret = -ENOMEM;
		goto fail_out;
	}

	core->name = kstrdup_const(init->name, GFP_KERNEL);
	if (!core->name) {
		ret = -ENOMEM;
		goto fail_name;
	}

	if (WARN_ON(!init->ops)) {
		ret = -EINVAL;
		goto fail_ops;
	}
	core->ops = init->ops;

	if (dev && pm_runtime_enabled(dev))
		core->rpm_enabled = true;
	core->dev = dev;
	core->of_node = np;
	if (dev && dev->driver)
		core->owner = dev->driver->owner;
	core->hw = hw;
	core->flags = init->flags;
	core->num_parents = init->num_parents;
	core->min_rate = 0;
	core->max_rate = ULONG_MAX;
	hw->core = core;

/*
 * IAMROOT, 2022.08.13:
 * - core의 parent map 할당 및 초기화.
 */
	ret = clk_core_populate_parent_map(core, init);
	if (ret)
		goto fail_parents;

	INIT_HLIST_HEAD(&core->clks);

	/*
	 * Don't call clk_hw_create_clk() here because that would pin the
	 * provider module to itself and prevent it from ever being removed.
	 */

/*
 * IAMROOT, 2022.08.13:
 * - clk 자료구조 설정.
 */
	hw->clk = alloc_clk(core, NULL, NULL);
	if (IS_ERR(hw->clk)) {
		ret = PTR_ERR(hw->clk);
		goto fail_create_clk;
	}

/*
 * IAMROOT, 2022.08.06:
 * - 자기자신으로 등록한다.
 *    +-- hw --+
 *    v        v
 *   core <--- clk
 */
	clk_core_link_consumer(hw->core, hw->clk);

/*
 * IAMROOT, 2022.08.13:
 * - core를 등록한다.
 */
	ret = __clk_core_init(core);
	if (!ret)
		return hw->clk;

/*
 * IAMROOT, 2022.08.06:
 * - 실패시 unlink후 free.
 */
	clk_prepare_lock();
	clk_core_unlink_consumer(hw->clk);
	clk_prepare_unlock();

	free_clk(hw->clk);
	hw->clk = NULL;

fail_create_clk:
	clk_core_free_parent_map(core);
fail_parents:
fail_ops:
	kfree_const(core->name);
fail_name:
	kfree(core);
fail_out:
	return ERR_PTR(ret);
}

/**
 * dev_or_parent_of_node() - Get device node of @dev or @dev's parent
 * @dev: Device to get device node of
 *
 * Return: device node pointer of @dev, or the device node pointer of
 * @dev->parent if dev doesn't have a device node, or NULL if neither
 * @dev or @dev->parent have a device node.
 */
static struct device_node *dev_or_parent_of_node(struct device *dev)
{
	struct device_node *np;

	if (!dev)
		return NULL;

	np = dev_of_node(dev);
	if (!np)
		np = dev_of_node(dev->parent);

	return np;
}

/**
 * clk_register - allocate a new clock, register it and return an opaque cookie
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * clk_register is the *deprecated* interface for populating the clock tree with
 * new clock nodes. Use clk_hw_register() instead.
 *
 * Returns: a pointer to the newly allocated struct clk which
 * cannot be dereferenced by driver code but may be used in conjunction with the
 * rest of the clock API.  In the event of an error clk_register will return an
 * error code; drivers must test for an error code after calling clk_register.
 */
struct clk *clk_register(struct device *dev, struct clk_hw *hw)
{
	return __clk_register(dev, dev_or_parent_of_node(dev), hw);
}
EXPORT_SYMBOL_GPL(clk_register);

/**
 * clk_hw_register - register a clk_hw and return an error code
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * clk_hw_register is the primary interface for populating the clock tree with
 * new clock nodes. It returns an integer equal to zero indicating success or
 * less than zero indicating failure. Drivers must test for an error code after
 * calling clk_hw_register().
 */
int clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	return PTR_ERR_OR_ZERO(__clk_register(dev, dev_or_parent_of_node(dev),
			       hw));
}
EXPORT_SYMBOL_GPL(clk_hw_register);

/*
 * of_clk_hw_register - register a clk_hw and return an error code
 * @node: device_node of device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * of_clk_hw_register() is the primary interface for populating the clock tree
 * with new clock nodes when a struct device is not available, but a struct
 * device_node is. It returns an integer equal to zero indicating success or
 * less than zero indicating failure. Drivers must test for an error code after
 * calling of_clk_hw_register().
 */

/*
 * IAMROOT, 2022.08.06:
 * - @hw로 받은 data로 core와 clk를 생성하고,
 *   root list or orphan list, 부모의 child 에 등록한다.
 */
int of_clk_hw_register(struct device_node *node, struct clk_hw *hw)
{
	return PTR_ERR_OR_ZERO(__clk_register(NULL, node, hw));
}
EXPORT_SYMBOL_GPL(of_clk_hw_register);

/* Free memory allocated for a clock. */
static void __clk_release(struct kref *ref)
{
	struct clk_core *core = container_of(ref, struct clk_core, ref);

	lockdep_assert_held(&prepare_lock);

	clk_core_free_parent_map(core);
	kfree_const(core->name);
	kfree(core);
}

/*
 * Empty clk_ops for unregistered clocks. These are used temporarily
 * after clk_unregister() was called on a clock and until last clock
 * consumer calls clk_put() and the struct clk object is freed.
 */
static int clk_nodrv_prepare_enable(struct clk_hw *hw)
{
	return -ENXIO;
}

static void clk_nodrv_disable_unprepare(struct clk_hw *hw)
{
	WARN_ON_ONCE(1);
}

static int clk_nodrv_set_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	return -ENXIO;
}

static int clk_nodrv_set_parent(struct clk_hw *hw, u8 index)
{
	return -ENXIO;
}

static const struct clk_ops clk_nodrv_ops = {
	.enable		= clk_nodrv_prepare_enable,
	.disable	= clk_nodrv_disable_unprepare,
	.prepare	= clk_nodrv_prepare_enable,
	.unprepare	= clk_nodrv_disable_unprepare,
	.set_rate	= clk_nodrv_set_rate,
	.set_parent	= clk_nodrv_set_parent,
};

static void clk_core_evict_parent_cache_subtree(struct clk_core *root,
						struct clk_core *target)
{
	int i;
	struct clk_core *child;

	for (i = 0; i < root->num_parents; i++)
		if (root->parents[i].core == target)
			root->parents[i].core = NULL;

	hlist_for_each_entry(child, &root->children, child_node)
		clk_core_evict_parent_cache_subtree(child, target);
}

/* Remove this clk from all parent caches */
static void clk_core_evict_parent_cache(struct clk_core *core)
{
	struct hlist_head **lists;
	struct clk_core *root;

	lockdep_assert_held(&prepare_lock);

	for (lists = all_lists; *lists; lists++)
		hlist_for_each_entry(root, *lists, child_node)
			clk_core_evict_parent_cache_subtree(root, core);

}

/**
 * clk_unregister - unregister a currently registered clock
 * @clk: clock to unregister
 */
void clk_unregister(struct clk *clk)
{
	unsigned long flags;
	const struct clk_ops *ops;

	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_debug_unregister(clk->core);

	clk_prepare_lock();

	ops = clk->core->ops;
	if (ops == &clk_nodrv_ops) {
		pr_err("%s: unregistered clock: %s\n", __func__,
		       clk->core->name);
		goto unlock;
	}
	/*
	 * Assign empty clock ops for consumers that might still hold
	 * a reference to this clock.
	 */
	flags = clk_enable_lock();
	clk->core->ops = &clk_nodrv_ops;
	clk_enable_unlock(flags);

	if (ops->terminate)
		ops->terminate(clk->core->hw);

	if (!hlist_empty(&clk->core->children)) {
		struct clk_core *child;
		struct hlist_node *t;

		/* Reparent all children to the orphan list. */
		hlist_for_each_entry_safe(child, t, &clk->core->children,
					  child_node)
			clk_core_set_parent_nolock(child, NULL);
	}

	clk_core_evict_parent_cache(clk->core);

	hlist_del_init(&clk->core->child_node);

	if (clk->core->prepare_count)
		pr_warn("%s: unregistering prepared clock: %s\n",
					__func__, clk->core->name);

	if (clk->core->protect_count)
		pr_warn("%s: unregistering protected clock: %s\n",
					__func__, clk->core->name);

	kref_put(&clk->core->ref, __clk_release);
	free_clk(clk);
unlock:
	clk_prepare_unlock();
}
EXPORT_SYMBOL_GPL(clk_unregister);

/**
 * clk_hw_unregister - unregister a currently registered clk_hw
 * @hw: hardware-specific clock data to unregister
 */
void clk_hw_unregister(struct clk_hw *hw)
{
	clk_unregister(hw->clk);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister);

static void devm_clk_unregister_cb(struct device *dev, void *res)
{
	clk_unregister(*(struct clk **)res);
}

static void devm_clk_hw_unregister_cb(struct device *dev, void *res)
{
	clk_hw_unregister(*(struct clk_hw **)res);
}

/**
 * devm_clk_register - resource managed clk_register()
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * Managed clk_register(). This function is *deprecated*, use devm_clk_hw_register() instead.
 *
 * Clocks returned from this function are automatically clk_unregister()ed on
 * driver detach. See clk_register() for more information.
 */
struct clk *devm_clk_register(struct device *dev, struct clk_hw *hw)
{
	struct clk *clk;
	struct clk **clkp;

	clkp = devres_alloc(devm_clk_unregister_cb, sizeof(*clkp), GFP_KERNEL);
	if (!clkp)
		return ERR_PTR(-ENOMEM);

	clk = clk_register(dev, hw);
	if (!IS_ERR(clk)) {
		*clkp = clk;
		devres_add(dev, clkp);
	} else {
		devres_free(clkp);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(devm_clk_register);

/**
 * devm_clk_hw_register - resource managed clk_hw_register()
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * Managed clk_hw_register(). Clocks registered by this function are
 * automatically clk_hw_unregister()ed on driver detach. See clk_hw_register()
 * for more information.
 */
int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	struct clk_hw **hwp;
	int ret;

	hwp = devres_alloc(devm_clk_hw_unregister_cb, sizeof(*hwp), GFP_KERNEL);
	if (!hwp)
		return -ENOMEM;

	ret = clk_hw_register(dev, hw);
	if (!ret) {
		*hwp = hw;
		devres_add(dev, hwp);
	} else {
		devres_free(hwp);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register);

static int devm_clk_match(struct device *dev, void *res, void *data)
{
	struct clk *c = res;
	if (WARN_ON(!c))
		return 0;
	return c == data;
}

static int devm_clk_hw_match(struct device *dev, void *res, void *data)
{
	struct clk_hw *hw = res;

	if (WARN_ON(!hw))
		return 0;
	return hw == data;
}

/**
 * devm_clk_unregister - resource managed clk_unregister()
 * @dev: device that is unregistering the clock data
 * @clk: clock to unregister
 *
 * Deallocate a clock allocated with devm_clk_register(). Normally
 * this function will not need to be called and the resource management
 * code will ensure that the resource is freed.
 */
void devm_clk_unregister(struct device *dev, struct clk *clk)
{
	WARN_ON(devres_release(dev, devm_clk_unregister_cb, devm_clk_match, clk));
}
EXPORT_SYMBOL_GPL(devm_clk_unregister);

/**
 * devm_clk_hw_unregister - resource managed clk_hw_unregister()
 * @dev: device that is unregistering the hardware-specific clock data
 * @hw: link to hardware-specific clock data
 *
 * Unregister a clk_hw registered with devm_clk_hw_register(). Normally
 * this function will not need to be called and the resource management
 * code will ensure that the resource is freed.
 */
void devm_clk_hw_unregister(struct device *dev, struct clk_hw *hw)
{
	WARN_ON(devres_release(dev, devm_clk_hw_unregister_cb, devm_clk_hw_match,
				hw));
}
EXPORT_SYMBOL_GPL(devm_clk_hw_unregister);

static void devm_clk_release(struct device *dev, void *res)
{
	clk_put(*(struct clk **)res);
}

/**
 * devm_clk_hw_get_clk - resource managed clk_hw_get_clk()
 * @dev: device that is registering this clock
 * @hw: clk_hw associated with the clk being consumed
 * @con_id: connection ID string on device
 *
 * Managed clk_hw_get_clk(). Clocks got with this function are
 * automatically clk_put() on driver detach. See clk_put()
 * for more information.
 */
struct clk *devm_clk_hw_get_clk(struct device *dev, struct clk_hw *hw,
				const char *con_id)
{
	struct clk *clk;
	struct clk **clkp;

	/* This should not happen because it would mean we have drivers
	 * passing around clk_hw pointers instead of having the caller use
	 * proper clk_get() style APIs
	 */
	WARN_ON_ONCE(dev != hw->core->dev);

	clkp = devres_alloc(devm_clk_release, sizeof(*clkp), GFP_KERNEL);
	if (!clkp)
		return ERR_PTR(-ENOMEM);

	clk = clk_hw_get_clk(hw, con_id);
	if (!IS_ERR(clk)) {
		*clkp = clk;
		devres_add(dev, clkp);
	} else {
		devres_free(clkp);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(devm_clk_hw_get_clk);

/*
 * clkdev helpers
 */

void __clk_put(struct clk *clk)
{
	struct module *owner;

	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_prepare_lock();

	/*
	 * Before calling clk_put, all calls to clk_rate_exclusive_get() from a
	 * given user should be balanced with calls to clk_rate_exclusive_put()
	 * and by that same consumer
	 */
	if (WARN_ON(clk->exclusive_count)) {
		/* We voiced our concern, let's sanitize the situation */
		clk->core->protect_count -= (clk->exclusive_count - 1);
		clk_core_rate_unprotect(clk->core);
		clk->exclusive_count = 0;
	}

	hlist_del(&clk->clks_node);
	if (clk->min_rate > clk->core->req_rate ||
	    clk->max_rate < clk->core->req_rate)
		clk_core_set_rate_nolock(clk->core, clk->core->req_rate);

	owner = clk->core->owner;
	kref_put(&clk->core->ref, __clk_release);

	clk_prepare_unlock();

	module_put(owner);

	free_clk(clk);
}

/***        clk rate change notifiers        ***/

/**
 * clk_notifier_register - add a clk rate change notifier
 * @clk: struct clk * to watch
 * @nb: struct notifier_block * with callback info
 *
 * Request notification when clk's rate changes.  This uses an SRCU
 * notifier because we want it to block and notifier unregistrations are
 * uncommon.  The callbacks associated with the notifier must not
 * re-enter into the clk framework by calling any top-level clk APIs;
 * this will cause a nested prepare_lock mutex.
 *
 * In all notification cases (pre, post and abort rate change) the original
 * clock rate is passed to the callback via struct clk_notifier_data.old_rate
 * and the new frequency is passed via struct clk_notifier_data.new_rate.
 *
 * clk_notifier_register() must be called from non-atomic context.
 * Returns -EINVAL if called with null arguments, -ENOMEM upon
 * allocation failure; otherwise, passes along the return value of
 * srcu_notifier_chain_register().
 */
/*
 * IAMROOT, 2022.08.13:
 * - @clk에 해당하는 @nb를 clk_notifier_list에 등록한다.
 */
int clk_notifier_register(struct clk *clk, struct notifier_block *nb)
{
	struct clk_notifier *cn;
	int ret = -ENOMEM;

	if (!clk || !nb)
		return -EINVAL;

	clk_prepare_lock();

	/* search the list of notifiers for this clk */
/*
 * IAMROOT, 2022.08.13:
 * - 이미 있는지 찾는다.
 */
	list_for_each_entry(cn, &clk_notifier_list, node)
		if (cn->clk == clk)
			goto found;

/*
 * IAMROOT, 2022.08.13:
 * - 없다면 메모리를 할당하고 초기화한후 clk_notifier_list에 넣고 notifier_count를
 *   증가시킨다.
 */
	/* if clk wasn't in the notifier list, allocate new clk_notifier */
	cn = kzalloc(sizeof(*cn), GFP_KERNEL);
	if (!cn)
		goto out;

	cn->clk = clk;
	srcu_init_notifier_head(&cn->notifier_head);

	list_add(&cn->node, &clk_notifier_list);

found:
	ret = srcu_notifier_chain_register(&cn->notifier_head, nb);

	clk->core->notifier_count++;

out:
	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_notifier_register);

/**
 * clk_notifier_unregister - remove a clk rate change notifier
 * @clk: struct clk *
 * @nb: struct notifier_block * with callback info
 *
 * Request no further notification for changes to 'clk' and frees memory
 * allocated in clk_notifier_register.
 *
 * Returns -EINVAL if called with null arguments; otherwise, passes
 * along the return value of srcu_notifier_chain_unregister().
 */
int clk_notifier_unregister(struct clk *clk, struct notifier_block *nb)
{
	struct clk_notifier *cn;
	int ret = -ENOENT;

	if (!clk || !nb)
		return -EINVAL;

	clk_prepare_lock();

	list_for_each_entry(cn, &clk_notifier_list, node) {
		if (cn->clk == clk) {
			ret = srcu_notifier_chain_unregister(&cn->notifier_head, nb);

			clk->core->notifier_count--;

			/* XXX the notifier code should handle this better */
			if (!cn->notifier_head.head) {
				srcu_cleanup_notifier_head(&cn->notifier_head);
				list_del(&cn->node);
				kfree(cn);
			}
			break;
		}
	}

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_notifier_unregister);

struct clk_notifier_devres {
	struct clk *clk;
	struct notifier_block *nb;
};

static void devm_clk_notifier_release(struct device *dev, void *res)
{
	struct clk_notifier_devres *devres = res;

	clk_notifier_unregister(devres->clk, devres->nb);
}

int devm_clk_notifier_register(struct device *dev, struct clk *clk,
			       struct notifier_block *nb)
{
	struct clk_notifier_devres *devres;
	int ret;

	devres = devres_alloc(devm_clk_notifier_release,
			      sizeof(*devres), GFP_KERNEL);

	if (!devres)
		return -ENOMEM;

	ret = clk_notifier_register(clk, nb);
	if (!ret) {
		devres->clk = clk;
		devres->nb = nb;
	} else {
		devres_free(devres);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_clk_notifier_register);

#ifdef CONFIG_OF

/*
 * IAMROOT, 2022.08.06:
 * - orphans에 대해서 reparent를 수행한다.
 */
static void clk_core_reparent_orphans(void)
{
	clk_prepare_lock();
	clk_core_reparent_orphans_nolock();
	clk_prepare_unlock();
}

/**
 * struct of_clk_provider - Clock provider registration structure
 * @link: Entry in global list of clock providers
 * @node: Pointer to device tree node of clock provider
 * @get: Get clock callback.  Returns NULL or a struct clk for the
 *       given clock specifier
 * @get_hw: Get clk_hw callback.  Returns NULL, ERR_PTR or a
 *       struct clk_hw for the given clock specifier
 * @data: context pointer to be passed into @get callback
 */
struct of_clk_provider {
	struct list_head link;

	struct device_node *node;
	struct clk *(*get)(struct of_phandle_args *clkspec, void *data);
	struct clk_hw *(*get_hw)(struct of_phandle_args *clkspec, void *data);
	void *data;
};

/*
 * IAMROOT, 2022.08.06:
 * - CLK_OF_DECLARE(..) 을 통해서 정의된 of_table들은
 *   __clk_of_table section에 struct of_device_id 형태로 compile time에 넣어진다.
 *   __clk_of_table은 해당 시작주소를 가리킨다.
 */
extern struct of_device_id __clk_of_table;
static const struct of_device_id __clk_of_table_sentinel
	__used __section("__clk_of_table_end");

static LIST_HEAD(of_clk_providers);
static DEFINE_MUTEX(of_clk_mutex);

struct clk *of_clk_src_simple_get(struct of_phandle_args *clkspec,
				     void *data)
{
	return data;
}
EXPORT_SYMBOL_GPL(of_clk_src_simple_get);

struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *clkspec, void *data)
{
	return data;
}
EXPORT_SYMBOL_GPL(of_clk_hw_simple_get);

struct clk *of_clk_src_onecell_get(struct of_phandle_args *clkspec, void *data)
{
	struct clk_onecell_data *clk_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= clk_data->clk_num) {
		pr_err("%s: invalid clock index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return clk_data->clks[idx];
}
EXPORT_SYMBOL_GPL(of_clk_src_onecell_get);

struct clk_hw *
of_clk_hw_onecell_get(struct of_phandle_args *clkspec, void *data)
{
	struct clk_hw_onecell_data *hw_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= hw_data->num) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return hw_data->hws[idx];
}
EXPORT_SYMBOL_GPL(of_clk_hw_onecell_get);

/**
 * of_clk_add_provider() - Register a clock provider for a node
 * @np: Device node pointer associated with clock provider
 * @clk_src_get: callback for decoding clock
 * @data: context pointer for @clk_src_get callback.
 *
 * This function is *deprecated*. Use of_clk_add_hw_provider() instead.
 */
int of_clk_add_provider(struct device_node *np,
			struct clk *(*clk_src_get)(struct of_phandle_args *clkspec,
						   void *data),
			void *data)
{
	struct of_clk_provider *cp;
	int ret;

	if (!np)
		return 0;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->node = of_node_get(np);
	cp->data = data;
	cp->get = clk_src_get;

	mutex_lock(&of_clk_mutex);
	list_add(&cp->link, &of_clk_providers);
	mutex_unlock(&of_clk_mutex);
	pr_debug("Added clock from %pOF\n", np);

	clk_core_reparent_orphans();

	ret = of_clk_set_defaults(np, true);
	if (ret < 0)
		of_clk_del_provider(np);

	fwnode_dev_initialized(&np->fwnode, true);

	return ret;
}
EXPORT_SYMBOL_GPL(of_clk_add_provider);

/**
 * of_clk_add_hw_provider() - Register a clock provider for a node
 * @np: Device node pointer associated with clock provider
 * @get: callback for decoding clk_hw
 * @data: context pointer for @get callback.
 */
/*
 * IAMROOT, 2022.08.06:
 * - of_clk_providers에 clk provider로 등록한다.
 */
int of_clk_add_hw_provider(struct device_node *np,
			   struct clk_hw *(*get)(struct of_phandle_args *clkspec,
						 void *data),
			   void *data)
{
	struct of_clk_provider *cp;
	int ret;

	if (!np)
		return 0;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->node = of_node_get(np);
	cp->data = data;
	cp->get_hw = get;

	mutex_lock(&of_clk_mutex);
	list_add(&cp->link, &of_clk_providers);
	mutex_unlock(&of_clk_mutex);
	pr_debug("Added clk_hw provider from %pOF\n", np);

	clk_core_reparent_orphans();

	ret = of_clk_set_defaults(np, true);
	if (ret < 0)
		of_clk_del_provider(np);

	fwnode_dev_initialized(&np->fwnode, true);

	return ret;
}
EXPORT_SYMBOL_GPL(of_clk_add_hw_provider);

static void devm_of_clk_release_provider(struct device *dev, void *res)
{
	of_clk_del_provider(*(struct device_node **)res);
}

/*
 * We allow a child device to use its parent device as the clock provider node
 * for cases like MFD sub-devices where the child device driver wants to use
 * devm_*() APIs but not list the device in DT as a sub-node.
 */
static struct device_node *get_clk_provider_node(struct device *dev)
{
	struct device_node *np, *parent_np;

	np = dev->of_node;
	parent_np = dev->parent ? dev->parent->of_node : NULL;

	if (!of_find_property(np, "#clock-cells", NULL))
		if (of_find_property(parent_np, "#clock-cells", NULL))
			np = parent_np;

	return np;
}

/**
 * devm_of_clk_add_hw_provider() - Managed clk provider node registration
 * @dev: Device acting as the clock provider (used for DT node and lifetime)
 * @get: callback for decoding clk_hw
 * @data: context pointer for @get callback
 *
 * Registers clock provider for given device's node. If the device has no DT
 * node or if the device node lacks of clock provider information (#clock-cells)
 * then the parent device's node is scanned for this information. If parent node
 * has the #clock-cells then it is used in registration. Provider is
 * automatically released at device exit.
 *
 * Return: 0 on success or an errno on failure.
 */
int devm_of_clk_add_hw_provider(struct device *dev,
			struct clk_hw *(*get)(struct of_phandle_args *clkspec,
					      void *data),
			void *data)
{
	struct device_node **ptr, *np;
	int ret;

	ptr = devres_alloc(devm_of_clk_release_provider, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	np = get_clk_provider_node(dev);
	ret = of_clk_add_hw_provider(np, get, data);
	if (!ret) {
		*ptr = np;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_of_clk_add_hw_provider);

/**
 * of_clk_del_provider() - Remove a previously registered clock provider
 * @np: Device node pointer associated with clock provider
 */
void of_clk_del_provider(struct device_node *np)
{
	struct of_clk_provider *cp;

	if (!np)
		return;

	mutex_lock(&of_clk_mutex);
	list_for_each_entry(cp, &of_clk_providers, link) {
		if (cp->node == np) {
			list_del(&cp->link);
			fwnode_dev_initialized(&np->fwnode, false);
			of_node_put(cp->node);
			kfree(cp);
			break;
		}
	}
	mutex_unlock(&of_clk_mutex);
}
EXPORT_SYMBOL_GPL(of_clk_del_provider);

static int devm_clk_provider_match(struct device *dev, void *res, void *data)
{
	struct device_node **np = res;

	if (WARN_ON(!np || !*np))
		return 0;

	return *np == data;
}

/**
 * devm_of_clk_del_provider() - Remove clock provider registered using devm
 * @dev: Device to whose lifetime the clock provider was bound
 */
void devm_of_clk_del_provider(struct device *dev)
{
	int ret;
	struct device_node *np = get_clk_provider_node(dev);

	ret = devres_release(dev, devm_of_clk_release_provider,
			     devm_clk_provider_match, np);

	WARN_ON(ret);
}
EXPORT_SYMBOL(devm_of_clk_del_provider);

/**
 * of_parse_clkspec() - Parse a DT clock specifier for a given device node
 * @np: device node to parse clock specifier from
 * @index: index of phandle to parse clock out of. If index < 0, @name is used
 * @name: clock name to find and parse. If name is NULL, the index is used
 * @out_args: Result of parsing the clock specifier
 *
 * Parses a device node's "clocks" and "clock-names" properties to find the
 * phandle and cells for the index or name that is desired. The resulting clock
 * specifier is placed into @out_args, or an errno is returned when there's a
 * parsing error. The @index argument is ignored if @name is non-NULL.
 *
 * Example:
 *
 * phandle1: clock-controller@1 {
 *	#clock-cells = <2>;
 * }
 *
 * phandle2: clock-controller@2 {
 *	#clock-cells = <1>;
 * }
 *
 * clock-consumer@3 {
 *	clocks = <&phandle1 1 2 &phandle2 3>;
 *	clock-names = "name1", "name2";
 * }
 *
 * To get a device_node for `clock-controller@2' node you may call this
 * function a few different ways:
 *
 *   of_parse_clkspec(clock-consumer@3, -1, "name2", &args);
 *   of_parse_clkspec(clock-consumer@3, 1, NULL, &args);
 *   of_parse_clkspec(clock-consumer@3, 1, "name2", &args);
 *
 * Return: 0 upon successfully parsing the clock specifier. Otherwise, -ENOENT
 * if @name is NULL or -EINVAL if @name is non-NULL and it can't be found in
 * the "clock-names" property of @np.
 */

/*
 * IAMROOT, 2022.08.06:
 * - ex)  imx8-ss-audio.dtsi 참고
 *   dsp라는 np를 예제로 가져온다;
 *   dsp: dsp@596e8000 {
 *	compatible = "fsl,imx8qxp-dsp";
 *	reg = <0x596e8000 0x88000>;
 *	clocks = <&dsp_lpcg IMX_LPCG_CLK_5>,
 *	<&dsp_ram_lpcg IMX_LPCG_CLK_4>,
 *	<&dsp_lpcg IMX_LPCG_CLK_7>;
 *	clock-names = "ipg", "ocram", "core";
 *   };
 *   clocks : 0번 index dsp_lpcg으로 사용하고 IMX_LPCG_CLK_5라는 속성을 사용한단것.
 *            1번 index dsp_ram_lpcg, 2번 index는 dsp_lpcg가 된다.
 *   clock-names : cloks의 index에 대응하는 각각의 이름
 *
 * - @name이나 @index로 clkspec을 찾는다. 만약 해당 np에서 못찾았고, parent np에
 *   clock-ranges;가 있다면 부모에서 @name, @index로 찾아본다.(for arm lagacy)
 */
static int of_parse_clkspec(const struct device_node *np, int index,
			    const char *name, struct of_phandle_args *out_args)
{
	int ret = -ENOENT;

	/* Walk up the tree of devices looking for a clock property that matches */
	while (np) {
		/*
		 * For named clocks, first look up the name in the
		 * "clock-names" property.  If it cannot be found, then index
		 * will be an error code and of_parse_phandle_with_args() will
		 * return -EINVAL.
		 */
/*
 * IAMROOT, 2022.08.06:
 * - name이 주어지면 name으로 index를 구해온다.
 *   cloks = <&dsp_lpcg ..>, <&dsp_ram_lpcg ..>, <&dsp_lpcg ..>
 *   clock-names = "ipg", "ocram", "core";
 *   라고 했을대 "ocram"로 인자가오면 <&dsp_ram_lpcg ..>을 찾아주는 개념.
 * - <&dsp_ram_lpcg IMX_LPCG_CLK_4>가 검색했다고 됬을대 out_args에 cnt는 1,
 *   arg[] = {IMX_LPCG_CLK_4} 가 얻어진다.
 */
		if (name)
			index = of_property_match_string(np, "clock-names", name);
		ret = of_parse_phandle_with_args(np, "clocks", "#clock-cells",
						 index, out_args);
/*
 * IAMROOT, 2022.08.06:
 * - 찾아지면 종료.
 */
		if (!ret)
			break;

/*
 * IAMROOT, 2022.08.06:
 * - 못찾았는데 name이 주어지고, name에 해당하는 index까지 찾아진 경우 종료(error)
 */
		if (name && index >= 0)
			break;

		/*
		 * No matching clock found on this node.  If the parent node
		 * has a "clock-ranges" property, then we can try one of its
		 * clocks.
		 */
/*
 * IAMROOT, 2022.08.06:
 * - 부모가 존재하고 부모가 clock-ranges를 찾는다. 없으면 break.(error)
 *   부모에 clock-ranges;가 있으면 부모까지 확장해서 name이나 index로 해보라는의미.
 */
		np = np->parent;
		if (np && !of_get_property(np, "clock-ranges", NULL))
			break;
		index = 0;
	}

	return ret;
}

static struct clk_hw *
__of_clk_get_hw_from_provider(struct of_clk_provider *provider,
			      struct of_phandle_args *clkspec)
{
	struct clk *clk;

/*
 * IAMROOT, 2022.08.06:
 * - ex) of_clk_hw_simple_get
 */
	if (provider->get_hw)
		return provider->get_hw(clkspec, provider->data);

	clk = provider->get(clkspec, provider->data);
	if (IS_ERR(clk))
		return ERR_CAST(clk);
	return __clk_get_hw(clk);
}

/*
 * IAMROOT, 2022.08.06:
 * - of_clk_providers에서 @clkspec이 존재하는 찾아본다
 * - 못찾으면 -EPROBE_DEFER
 */
static struct clk_hw *
of_clk_get_hw_from_clkspec(struct of_phandle_args *clkspec)
{
	struct of_clk_provider *provider;
	struct clk_hw *hw = ERR_PTR(-EPROBE_DEFER);

	if (!clkspec)
		return ERR_PTR(-EINVAL);

	mutex_lock(&of_clk_mutex);
	list_for_each_entry(provider, &of_clk_providers, link) {
		if (provider->node == clkspec->np) {
			hw = __of_clk_get_hw_from_provider(provider, clkspec);
			if (!IS_ERR(hw))
				break;
		}
	}
	mutex_unlock(&of_clk_mutex);

	return hw;
}

/**
 * of_clk_get_from_provider() - Lookup a clock from a clock provider
 * @clkspec: pointer to a clock specifier data structure
 *
 * This function looks up a struct clk from the registered list of clock
 * providers, an input is a clock specifier data structure as returned
 * from the of_parse_phandle_with_args() function call.
 */

/*
 * IAMROOT, 2022.08.06:
 * - papago
 *   이 함수는 등록된 클럭 제공자 목록에서 구조체 clk를 조회하며, 입력은
 *   of_parse_phandle_with_args() 함수 호출에서 반환된 클럭 지정자 데이터 구조입니다.
 */
struct clk *of_clk_get_from_provider(struct of_phandle_args *clkspec)
{
	struct clk_hw *hw = of_clk_get_hw_from_clkspec(clkspec);

	return clk_hw_create_clk(NULL, hw, NULL, __func__);
}
EXPORT_SYMBOL_GPL(of_clk_get_from_provider);

/*
 * IAMROOT, 2022.08.06:
 * - @np에서 @index, @con_id에 해당하는 clock을 찾은후
 *   of_clk_providers에서 일치하는게 있는지 확인한다.
 */
struct clk_hw *of_clk_get_hw(struct device_node *np, int index,
			     const char *con_id)
{
	int ret;
	struct clk_hw *hw;
	struct of_phandle_args clkspec;

/*
 * IAMROOT, 2022.08.06:
 * - @index or @con_id에 해당하는 clock을 phandle argu를 가져온다.
 */
	ret = of_parse_clkspec(np, index, con_id, &clkspec);
	if (ret)
		return ERR_PTR(ret);

/*
 * IAMROOT, 2022.08.06:
 * - of_clk_provider에서 일치하는 걸 찾는다. 못찾았다면 -EPROBE_DEFER
 */
	hw = of_clk_get_hw_from_clkspec(&clkspec);
	of_node_put(clkspec.np);

	return hw;
}

/*
 * IAMROOT, 2022.08.06:
 * - @con_id or @index에 해당하는 clk을 찾고 struct clk를 초기화하여
 *   clk core에 consumer로 등록한다.
 */
static struct clk *__of_clk_get(struct device_node *np,
				int index, const char *dev_id,
				const char *con_id)
{

/*
 * IAMROOT, 2022.08.06:
 * - @index or @con_idx에 해당하는 phandle에 대응하는 clk_hw를 찾고 of_clk_provider
 *   에서 검색을 한다.
 * - of_clk_provider에 없으면 -EPROBE_DEFER.
 */
	struct clk_hw *hw = of_clk_get_hw(np, index, con_id);

	return clk_hw_create_clk(NULL, hw, dev_id, con_id);
}

struct clk *of_clk_get(struct device_node *np, int index)
{
	return __of_clk_get(np, index, np->full_name, NULL);
}
EXPORT_SYMBOL(of_clk_get);

/**
 * of_clk_get_by_name() - Parse and lookup a clock referenced by a device node
 * @np: pointer to clock consumer node
 * @name: name of consumer's clock input, or NULL for the first clock reference
 *
 * This function parses the clocks and clock-names properties,
 * and uses them to look up the struct clk from the registered list of clock
 * providers.
 */
struct clk *of_clk_get_by_name(struct device_node *np, const char *name)
{
	if (!np)
		return ERR_PTR(-ENOENT);

	return __of_clk_get(np, 0, np->full_name, name);
}
EXPORT_SYMBOL(of_clk_get_by_name);

/**
 * of_clk_get_parent_count() - Count the number of clocks a device node has
 * @np: device node to count
 *
 * Returns: The number of clocks that are possible parents of this node
 */

/*
 * IAMROOT, 2022.08.13:
 * - clocks의 입력소스가 몇개잇ㅎ는지 알아온다.
 *   ex) clocks = <&a>, <&b> => 2개.
 */
unsigned int of_clk_get_parent_count(const struct device_node *np)
{
	int count;

	count = of_count_phandle_with_args(np, "clocks", "#clock-cells");
	if (count < 0)
		return 0;

	return count;
}
EXPORT_SYMBOL_GPL(of_clk_get_parent_count);

/*
 * IAMROOT, 2022.08.13:
 * - @index에 해당하는 clk의 parent name을 가져온다.
 */
const char *of_clk_get_parent_name(const struct device_node *np, int index)
{
	struct of_phandle_args clkspec;
	struct property *prop;
	const char *clk_name;
	const __be32 *vp;
	u32 pv;
	int rc;
	int count;
	struct clk *clk;

/*
 * IAMROOT, 2022.08.13:
 * - index에 해당하는 phandle을 가져온다.
 *   ex) clocks = <&a ..> <&b ..>
 *   index == 0이면 <&a ..>에 대한 phandle 가져온다.
 */
	rc = of_parse_phandle_with_args(np, "clocks", "#clock-cells", index,
					&clkspec);
	if (rc)
		return NULL;

/*
 * IAMROOT, 2022.08.13:
 * - clocks = <&a 2> <&b 3>
 *   index = 0 요청 이라고 했을때
 *   &a 2가 선택되고 여기서 args[0]는 2가 되어
 *   index = 2가 된다.
 */
	index = clkspec.args_count ? clkspec.args[0] : 0;
	count = 0;

	/* if there is an indices property, use it to transfer the index
	 * specified into an array offset for the clock-output-names property.
	 */
/*
 * IAMROOT, 2022.08.13:
 * - parent np에서 clock-indices로 index를 변경해야되는지 확인한다.
 *
 * - clock-indices = <3> <5>
 *   index 번호를 바꾼다는것.
 *   ex) clocks = <&a ..> <&b ..>
 *       clock-indices = <3> <5>
 *       a가 index = 0 -> 3, b가 index 1 -> 5로 바뀌게되는것이고 이 후 바뀐
 *       index로 접근해야된다.
 */
	of_property_for_each_u32(clkspec.np, "clock-indices", prop, vp, pv) {
		if (index == pv) {
			index = count;
			break;
		}
		count++;
	}
	/* We went off the end of 'clock-indices' without finding it */
	if (prop && !vp)
		return NULL;

/*
 * IAMROOT, 2022.08.13:
 * - parent np에서 정해진 이름이 있는지 확인한다.
 *   
 * - index로 안하고 문자열로 찾고싶을때 사용.
 */
	if (of_property_read_string_index(clkspec.np, "clock-output-names",
					  index,
					  &clk_name) < 0) {
		/*
		 * Best effort to get the name if the clock has been
		 * registered with the framework. If the clock isn't
		 * registered, we return the node name as the name of
		 * the clock as long as #clock-cells = 0.
		 */
/*
 * IAMROOT, 2022.08.13:
 * - 실패했거나 clock-output-names가 없는경우엔 clkspec로 등록된 provider에서
 *   찾아서 clk_name을 설정한다.
 */
		clk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(clk)) {
			if (clkspec.args_count == 0)
				clk_name = clkspec.np->name;
			else
				clk_name = NULL;
		} else {
			clk_name = __clk_get_name(clk);
			clk_put(clk);
		}
	}


	of_node_put(clkspec.np);
	return clk_name;
}
EXPORT_SYMBOL_GPL(of_clk_get_parent_name);

/**
 * of_clk_parent_fill() - Fill @parents with names of @np's parents and return
 * number of parents
 * @np: Device node pointer associated with clock provider
 * @parents: pointer to char array that hold the parents' names
 * @size: size of the @parents array
 *
 * Return: number of parents for the clock node.
 */
/*
 * IAMROOT, 2022.08.13:
 * - @np의 @size(parent 개수)만큼 순회해서 parents에 parent name을 채운다.
 */
int of_clk_parent_fill(struct device_node *np, const char **parents,
		       unsigned int size)
{
	unsigned int i = 0;

	while (i < size && (parents[i] = of_clk_get_parent_name(np, i)) != NULL)
		i++;

	return i;
}
EXPORT_SYMBOL_GPL(of_clk_parent_fill);

struct clock_provider {
	void (*clk_init_cb)(struct device_node *);
	struct device_node *np;
	struct list_head node;
};

/*
 * This function looks for a parent clock. If there is one, then it
 * checks that the provider for this parent clock was initialized, in
 * this case the parent clock will be ready.
 */

/*
 * IAMROOT, 2022.08.06:
 * -papgo
 *  이 함수는 부모 clock를 찾습니다. 있는 경우 이 상위 clock의 공급자가
 *  초기화되었는지 확인합니다. 이 경우 상위 clock가 준비됩니다. 
 *
 * - @np의 index 0부터 parent clock을 순회하며 clk을 가져온다.
 *
 * - 만약 -EPROBE_DEFER가 났다면 상위 부모를 못찾았다(부모가 아직 준비가 안됬다)
 *   는 의미가 되고 return 0.
 */
static int parent_ready(struct device_node *np)
{
	int i = 0;

	while (true) {
/*
 * IAMROOT, 2022.08.06:
 * - 
 */
		struct clk *clk = of_clk_get(np, i);

		/* this parent is ready we can check the next one */
		if (!IS_ERR(clk)) {
			clk_put(clk);
			i++;
			continue;
		}

		/* at least one parent is not ready, we exit now */
		if (PTR_ERR(clk) == -EPROBE_DEFER)
			return 0;

		/*
		 * Here we make assumption that the device tree is
		 * written correctly. So an error means that there is
		 * no more parent. As we didn't exit yet, then the
		 * previous parent are ready. If there is no clock
		 * parent, no need to wait for them, then we can
		 * consider their absence as being ready
		 */
/*
 * IAMROOT, 2022.08.06:
 * - papago
 *   여기서 우리는 장치 트리가 올바르게 작성되었다고 가정합니다. 따라서 오류는
 *   더 이상 부모가 없음을 의미합니다. 아직 종료하지 않았으므로 이전 부모가
 *   준비되었습니다. clock 부모가 없으면 기다릴 필요가 없다면 그들의 부재는 준비된
 *   것으로 간주할 수 있습니다.*
 */
		return 1;
	}
}

/**
 * of_clk_detect_critical() - set CLK_IS_CRITICAL flag from Device Tree
 * @np: Device node pointer associated with clock provider
 * @index: clock index
 * @flags: pointer to top-level framework flags
 *
 * Detects if the clock-critical property exists and, if so, sets the
 * corresponding CLK_IS_CRITICAL flag.
 *
 * Do not use this function. It exists only for legacy Device Tree
 * bindings, such as the one-clock-per-node style that are outdated.
 * Those bindings typically put all clock data into .dts and the Linux
 * driver has no clock data, thus making it impossible to set this flag
 * correctly from the driver. Only those drivers may call
 * of_clk_detect_critical from their setup functions.
 *
 * Return: error code or zero on success
 */
int of_clk_detect_critical(struct device_node *np, int index,
			   unsigned long *flags)
{
	struct property *prop;
	const __be32 *cur;
	uint32_t idx;

	if (!np || !flags)
		return -EINVAL;

	of_property_for_each_u32(np, "clock-critical", prop, cur, idx)
		if (index == idx)
			*flags |= CLK_IS_CRITICAL;

	return 0;
}

/**
 * of_clk_init() - Scan and init clock providers from the DT
 * @matches: array of compatible values and init functions for providers.
 *
 * This function scans the device tree for matching clock providers
 * and calls their initialization functions. It also does it by trying
 * to follow the dependencies.
 */
void __init of_clk_init(const struct of_device_id *matches)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct clock_provider *clk_provider, *next;
	bool is_init_done;
	bool force = false;
	LIST_HEAD(clk_provider_list);

/*
 * IAMROOT, 2022.08.06:
 * - matches == NULL일 경우 kernel image(CLK_OF_DECLARE(..))에 있는 구조체
 *   시작주소를 가져온다.
 *   ex) CLK_OF_DECLARE(fixed_clk, "fixed-clock", of_fixed_clk_setup);
 *   device tree에서 compatible = "fixed-clock"인 것들을 of_device_id로 가져온다.
 *
 * - kernel image에서 1, 2, 3, 4, 5 라는 of_device_id가 있고, dts에서 3, 4, 6라는
 *   device_node가 있다고 했을때, 매치되는 3, 4를 찾는 과정.
 */
	if (!matches)
		matches = &__clk_of_table;

	/* First prepare the list of the clocks providers */
	for_each_matching_node_and_match(np, matches, &match) {
		struct clock_provider *parent;

/*
 * IAMROOT, 2022.08.06:
 * - status ok가 아닌것들을 skip한다.
 */
		if (!of_device_is_available(np))
			continue;

		parent = kzalloc(sizeof(*parent), GFP_KERNEL);
		if (!parent) {
/*
 * IAMROOT, 2022.08.06:
 * - 할당 실패했을 경우 여지껏 만든것을 모두 해제하고 return.
 */
			list_for_each_entry_safe(clk_provider, next,
						 &clk_provider_list, node) {
				list_del(&clk_provider->node);
				of_node_put(clk_provider->np);
				kfree(clk_provider);
			}
			of_node_put(np);
			return;
		}

		parent->clk_init_cb = match->data;
		parent->np = of_node_get(np);
		list_add_tail(&parent->node, &clk_provider_list);
	}

/*
 * IAMROOT, 2022.08.06:
 * - parent_ready가 되는 것들은 먼저 처리하고, 그 다음 루틴에서 나머지를 처리한다.
 */
	while (!list_empty(&clk_provider_list)) {
		is_init_done = false;
		list_for_each_entry_safe(clk_provider, next,
					&clk_provider_list, node) {
			if (force || parent_ready(clk_provider->np)) {

				/* Don't populate platform devices */
				of_node_set_flag(clk_provider->np,
						 OF_POPULATED);

/*
 * IAMROOT, 2022.08.06:
 * - of_fixed_clk_setup(fixed rate등록). 참고.
 *   of_fixed_factor_clk_setup(fixed factor등록),
 *   of_clk_providers에 등록된다.
 * - of_clk_add_hw_provider이나 of_clk_add_provider를 통해서 등록될것이다.
 *
 * ex)
 *  읽힌 순서 |    3    2    1
 *  tree 순서 |    A -> B -> C
 *
 * step 1) C가 orphan으로 들어간다.
 *    root   |
 *    orphan | C
 *
 * step 2) B가 orphan으로 들어가고, C는 B의 child로 들어간다.
 *    root   |
 *    orphan | B
 *
 * step 3) A가 root로 들어가고, orphan list 검색을 통해 B가 A의 child로 들어가
 *             완성된다.
 *    root   | A
 *    orphan | 
 */
				clk_provider->clk_init_cb(clk_provider->np);
				of_clk_set_defaults(clk_provider->np, true);

				list_del(&clk_provider->node);
				of_node_put(clk_provider->np);
				kfree(clk_provider);
				is_init_done = true;
			}
		}

		/*
		 * We didn't manage to initialize any of the
		 * remaining providers during the last loop, so now we
		 * initialize all the remaining ones unconditionally
		 * in case the clock parent was not mandatory
		 */
/*
 * IAMROOT, 2022.08.06:
 * - 한번에 안되면 한번더 돈다. 한번더 돌때는 parent_ready를 안따른다.
 */
		if (!is_init_done)
			force = true;
	}
}
#endif
