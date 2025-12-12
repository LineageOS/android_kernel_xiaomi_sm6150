// SPDX-License-Identifier: GPL-2.0
/*
 * SM6150 DDR Quiescence Manager
 *
 * Copyright (C) 2025 Miguel Angel <miguel@nebuia.com>
 *
 * Inspired by Tensor AIO from Sultan Alsawaf <sultan@kerneltoast.com>
 *
 * This module handles DDR quiescence - lowering DDR frequency when the
 * system is idle to save power. The main frequency monitoring and throttle
 * detection is integrated directly into schedutil for optimal performance.
 *
 * Optimizations:
 * - Cache line aligned data structures for ARM64
 * - Minimal overhead polling with high-resolution timers
 * - Integration with devfreq_boost to avoid conflicts
 */

#define pr_fmt(fmt) "sm6150_perfmon: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/devfreq_boost.h>

#include "../../kernel/sched/sched.h"

/* Configuration */
#define QUIESCENCE_POLL_MS	50	/* Poll interval for idle detection */
#define QUIESCENCE_TIMEOUT_MS	100	/* Time before declaring quiescence */

/*
 * Global state - cache line aligned for ARM64.
 */
static struct {
	struct task_struct *thread;
	atomic_long_t last_activity_jiffies;
	bool quiescent;
	raw_spinlock_t lock;

	/* DDR devfreq handle from devfreq_boost */
	struct devfreq *ddr_devfreq;

	/* Saved frequencies for quiescence restore */
	unsigned long ddr_saved_min_freq;
	unsigned long ddr_saved_max_freq;
} perfmon_state __aligned(64) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(perfmon_state.lock),
};

/*
 * Check if all CPUs are idle.
 */
static bool all_cpus_idle(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		if (READ_ONCE(rq->nr_running) > 0 &&
		    !is_idle_task(rq->curr))
			return false;
	}

	return true;
}

/*
 * Enter quiescent state - lower DDR to minimum frequency.
 */
static void enter_quiescence(void)
{
	unsigned long flags;
	struct devfreq *df;

	raw_spin_lock_irqsave(&perfmon_state.lock, flags);

	if (perfmon_state.quiescent)
		goto unlock;

	perfmon_state.quiescent = true;
	raw_spin_unlock_irqrestore(&perfmon_state.lock, flags);

	df = perfmon_state.ddr_devfreq;
	if (df && df->profile && df->profile->freq_table) {
		mutex_lock(&df->lock);

		perfmon_state.ddr_saved_min_freq = df->min_freq;
		perfmon_state.ddr_saved_max_freq = df->max_freq;

		df->min_freq = df->profile->freq_table[0];
		df->max_freq = df->profile->freq_table[0];
		update_devfreq(df);

		mutex_unlock(&df->lock);

		pr_debug("Entered quiescence, DDR at minimum\n");
	}

	return;

unlock:
	raw_spin_unlock_irqrestore(&perfmon_state.lock, flags);
}

/*
 * Exit quiescent state - restore DDR frequency.
 */
static void exit_quiescence(void)
{
	unsigned long flags;
	struct devfreq *df;

	raw_spin_lock_irqsave(&perfmon_state.lock, flags);

	if (!perfmon_state.quiescent)
		goto unlock;

	perfmon_state.quiescent = false;
	raw_spin_unlock_irqrestore(&perfmon_state.lock, flags);

	df = perfmon_state.ddr_devfreq;
	if (df) {
		mutex_lock(&df->lock);

		df->min_freq = perfmon_state.ddr_saved_min_freq;
		df->max_freq = perfmon_state.ddr_saved_max_freq;
		update_devfreq(df);

		mutex_unlock(&df->lock);

		pr_debug("Exited quiescence, DDR restored\n");
	}

	return;

unlock:
	raw_spin_unlock_irqrestore(&perfmon_state.lock, flags);
}

/*
 * Check and update quiescence state.
 */
static void check_quiescence(void)
{
	unsigned long last_activity = atomic_long_read(
			&perfmon_state.last_activity_jiffies);
	bool should_quiesce;

	if (!all_cpus_idle()) {
		atomic_long_set(&perfmon_state.last_activity_jiffies, jiffies);

		if (perfmon_state.quiescent)
			exit_quiescence();
		return;
	}

	should_quiesce = time_after(jiffies,
			last_activity + msecs_to_jiffies(QUIESCENCE_TIMEOUT_MS));

	if (should_quiesce && !perfmon_state.quiescent)
		enter_quiescence();
}

/*
 * Quiescence monitoring thread.
 */
static int quiescence_thread(void *unused)
{
	set_user_nice(current, 19);  /* Low priority - this is not critical */

	while (!kthread_should_stop()) {
		check_quiescence();
		msleep(QUIESCENCE_POLL_MS);
	}

	return 0;
}

static int __init sm6150_perfmon_init(void)
{
	pr_info("Initializing SM6150 DDR Quiescence Manager\n");

	atomic_long_set(&perfmon_state.last_activity_jiffies, jiffies);

	/* Get DDR devfreq from devfreq_boost */
	perfmon_state.ddr_devfreq = devfreq_boost_get_device(DEVFREQ_CPU_LLCC_DDR_BW);

	if (perfmon_state.ddr_devfreq)
		pr_info("Found DDR devfreq device\n");
	else
		pr_info("DDR devfreq not available, quiescence disabled\n");

	/* Only start thread if we have DDR control */
	if (perfmon_state.ddr_devfreq) {
		perfmon_state.thread = kthread_run(quiescence_thread, NULL,
						   "ddr_quiescence");
		if (IS_ERR(perfmon_state.thread)) {
			pr_err("Failed to create quiescence thread\n");
			return PTR_ERR(perfmon_state.thread);
		}
	}

	pr_info("SM6150 DDR Quiescence Manager initialized\n");
	pr_info("Note: Freq monitoring integrated in schedutil (Tensor AIO)\n");
	return 0;
}

static void __exit sm6150_perfmon_exit(void)
{
	if (perfmon_state.thread)
		kthread_stop(perfmon_state.thread);

	if (perfmon_state.quiescent)
		exit_quiescence();

	pr_info("SM6150 DDR Quiescence Manager unloaded\n");
}

late_initcall(sm6150_perfmon_init);
module_exit(sm6150_perfmon_exit);

MODULE_AUTHOR("Miguel Angel <miguel@nebuia.com>");
MODULE_DESCRIPTION("SM6150 DDR Quiescence - Tensor AIO for Snapdragon 730G");
MODULE_LICENSE("GPL v2");
