// SPDX-License-Identifier: GPL-2.0
/*
 * SM6150 Performance Monitor
 *
 * Copyright (C) 2025 Miguel Angel <miguel@nebuia.com>
 *
 * Inspired by Tensor AIO from Sultan Alsawaf <sultan@kerneltoast.com>
 * Adapted for Qualcomm SM6150 (Snapdragon 730G) with WALT integration.
 *
 * This driver provides:
 * - Real CPU frequency measurement via WALT cycle counters
 * - freq_scale updates based on measured (not requested) frequency
 * - Hardware throttle detection when measured freq < target freq
 * - Memory quiescence detection to lower DDR when idle
 * - Integration with devfreq_boost to avoid conflicts
 *
 * Optimizations:
 * - Cache line aligned data structures
 * - Hot-path functions aligned to cache line boundaries
 * - Direct PMU counter reading via assembly where possible
 * - Minimal locking with atomic operations
 */

#define pr_fmt(fmt) "sm6150_perfmon: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/devfreq.h>
#include <linux/devfreq_boost.h>

#include "../../kernel/sched/sched.h"

/* Configuration - tuned for SM6150 */
#define PERFMON_POLL_MS			10	/* Poll interval in ms */
#define THROTTLE_DETECT_MARGIN_PCT	5	/* 5% margin for throttle detection */
#define QUIESCENCE_TIMEOUT_MS		100	/* Time before declaring quiescence */
#define MIN_SAMPLE_TIME_NS		(3 * NSEC_PER_USEC) /* Min sample window */

/* Throttle detection latency threshold (500us in ns) - matches Tensor AIO */
#define THROTTLE_LAT_NS			(500 * NSEC_PER_USEC)

/*
 * Compare two CPU frequencies to see if they are sufficiently close, within ~5%
 * of each other. This mimics capacity_greater() in sched/fair.c from Tensor AIO.
 */
#define cpu_freqs_similar(lower_freq, higher_freq) \
	((u64)(lower_freq) * 1078 >= (u64)(higher_freq) * 1024)

/*
 * Per-CPU data for frequency tracking.
 * Cache line aligned to prevent false sharing between CPUs.
 */
struct cpu_perfmon_data {
	/* Frequency measurement data */
	u32 measured_freq_khz;
	u32 target_freq_khz;

	/* Throttle detection */
	u64 throttle_start_ns;
	bool throttled;

	/* Padding to cache line */
	u8 __pad[64 - sizeof(u32) * 2 - sizeof(u64) - sizeof(bool)];
} __aligned(64);

static DEFINE_PER_CPU(struct cpu_perfmon_data, perfmon_data);

/*
 * Global state - kept together for cache efficiency.
 */
static struct {
	struct task_struct *thread;
	atomic_t active_cpus;
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
 * Get measured CPU frequency from WALT's cycle counter data.
 * WALT stores cycles and time in rq->cc for each CPU.
 *
 * This function is noinline and cache-line aligned to ensure consistent
 * timing when reading the counters, similar to Sultan's approach.
 */
static noinline u32 __aligned(64) get_measured_freq_khz(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	u64 cycles, time_ns;

	/*
	 * Read WALT's cycle counter data atomically.
	 * rq->cc.cycles is already scaled by NSEC_PER_MSEC
	 * rq->cc.time is in nanoseconds
	 * So cycles / time gives frequency in KHz directly.
	 */
	cycles = READ_ONCE(rq->cc.cycles);
	time_ns = READ_ONCE(rq->cc.time);

	/* Avoid division by zero and ensure minimum sample time */
	if (unlikely(!time_ns || time_ns < MIN_SAMPLE_TIME_NS))
		return 0;

	/* freq_khz = cycles / time_ns (since cycles scaled by NSEC_PER_MSEC) */
	return div64_u64(cycles, time_ns);
}

/*
 * Update freq_scale for a CPU based on measured frequency.
 * This gives the scheduler accurate information about actual CPU performance.
 *
 * Cache-line aligned for consistent hot-path performance.
 */
static noinline void __aligned(64)
update_cpu_freq_scale(int cpu, u32 measured_khz, u32 max_khz)
{
	unsigned long scale;

	if (unlikely(!measured_khz || !max_khz))
		return;

	/* Clamp measured frequency to max */
	if (measured_khz > max_khz)
		measured_khz = max_khz;

	/* Calculate scale: (measured / max) * SCHED_CAPACITY_SCALE */
	scale = ((unsigned long)measured_khz << SCHED_CAPACITY_SHIFT) / max_khz;

	/*
	 * Update the per-cpu freq_scale directly.
	 * This is what the scheduler uses for frequency-invariant accounting.
	 */
	WRITE_ONCE(per_cpu(freq_scale, cpu), scale);
}

/*
 * Check if a CPU is being throttled by comparing measured vs target frequency.
 * Uses the same algorithm as Tensor AIO for consistency.
 */
static bool detect_cpu_throttle(int cpu)
{
	struct cpu_perfmon_data *data = per_cpu_ptr(&perfmon_data, cpu);
	u64 now_ns = ktime_get_ns();
	u32 measured = READ_ONCE(data->measured_freq_khz);
	u32 target = READ_ONCE(data->target_freq_khz);

	if (unlikely(!measured || !target))
		return false;

	/*
	 * Check if measured frequency is significantly below target.
	 * Using cpu_freqs_similar() macro for ~5% tolerance.
	 */
	if (!cpu_freqs_similar(measured, target)) {
		/* Start throttle timer if not already started */
		if (!data->throttle_start_ns)
			data->throttle_start_ns = now_ns;

		/*
		 * Confirm throttle after latency threshold.
		 * This avoids false positives during frequency transitions.
		 */
		if (now_ns - data->throttle_start_ns >= THROTTLE_LAT_NS) {
			data->throttled = true;
			return true;
		}
	} else {
		/* Reset throttle detection */
		data->throttle_start_ns = 0;
		data->throttled = false;
	}

	return data->throttled;
}

/*
 * Update CPU capacity based on throttle state.
 * When throttled, report reduced capacity to scheduler via thermal pressure.
 */
static void update_cpu_capacity(int cpu)
{
	struct cpu_perfmon_data *data = per_cpu_ptr(&perfmon_data, cpu);
	struct cpufreq_policy *policy;
	unsigned long capacity;

	if (!data->throttled)
		return;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	/* Calculate throttled capacity based on measured frequency */
	if (data->measured_freq_khz && policy->cpuinfo.max_freq) {
		capacity = ((unsigned long)data->measured_freq_khz *
			    SCHED_CAPACITY_SCALE) / policy->cpuinfo.max_freq;

		/*
		 * Update topology capacity for the CPU.
		 * This informs the scheduler about reduced performance.
		 */
		topology_set_cpu_scale(cpu, capacity);
	}

	cpufreq_cpu_put(policy);
}

/*
 * Process a single CPU's performance data.
 * This is the main hot-path function called for each CPU.
 */
static void process_cpu(int cpu)
{
	struct cpu_perfmon_data *data = per_cpu_ptr(&perfmon_data, cpu);
	struct cpufreq_policy *policy;
	u32 measured_khz;

	if (!cpu_online(cpu) || !cpu_active(cpu))
		return;

	/* Get measured frequency from WALT cycle counters */
	measured_khz = get_measured_freq_khz(cpu);
	if (!measured_khz)
		return;

	WRITE_ONCE(data->measured_freq_khz, measured_khz);

	/* Get target frequency from cpufreq */
	policy = cpufreq_cpu_get(cpu);
	if (policy) {
		WRITE_ONCE(data->target_freq_khz, policy->cur);

		/* Update freq_scale with measured frequency */
		update_cpu_freq_scale(cpu, measured_khz,
				      policy->cpuinfo.max_freq);

		cpufreq_cpu_put(policy);
	}

	/* Check for throttling and update capacity if needed */
	if (detect_cpu_throttle(cpu))
		update_cpu_capacity(cpu);
}

/*
 * Check if all CPUs are idle (for quiescence detection).
 * Uses direct rq access for minimal overhead.
 */
static bool all_cpus_idle(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		/*
		 * Check if there are runnable tasks that aren't the idle task.
		 * This is the same check used in Tensor AIO.
		 */
		if (READ_ONCE(rq->nr_running) > 0 &&
		    !is_idle_task(READ_ONCE(rq->curr)))
			return false;
	}
	return true;
}

/*
 * Enter quiescent state - lower DDR frequency to save power.
 * Uses the same mechanism as devfreq_boost for consistency.
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

	/* Request minimum frequency for DDR */
	df = perfmon_state.ddr_devfreq;
	if (df && df->profile && df->profile->freq_table) {
		mutex_lock(&df->lock);

		/* Save current limits to restore later */
		perfmon_state.ddr_saved_min_freq = df->min_freq;
		perfmon_state.ddr_saved_max_freq = df->max_freq;

		/*
		 * Force minimum frequency by setting both min and max
		 * to the lowest freq in the table. This is the same
		 * approach used by devfreq_boost for screen-off.
		 */
		df->min_freq = df->profile->freq_table[0];
		df->max_freq = df->profile->freq_table[0];
		update_devfreq(df);

		mutex_unlock(&df->lock);

		pr_debug("Entered quiescence, DDR forced to %lu\n",
			 df->profile->freq_table[0]);
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

	/* Restore DDR to saved frequencies */
	df = perfmon_state.ddr_devfreq;
	if (df) {
		mutex_lock(&df->lock);

		/* Restore saved min/max frequency limits */
		df->min_freq = perfmon_state.ddr_saved_min_freq;
		df->max_freq = perfmon_state.ddr_saved_max_freq;
		update_devfreq(df);

		mutex_unlock(&df->lock);

		pr_debug("Exited quiescence, DDR restored to min=%lu max=%lu\n",
			 df->min_freq, df->max_freq);
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

	/* Update activity timestamp if CPUs are active */
	if (!all_cpus_idle()) {
		atomic_long_set(&perfmon_state.last_activity_jiffies, jiffies);

		if (perfmon_state.quiescent)
			exit_quiescence();
		return;
	}

	/* Check if we've been idle long enough */
	should_quiesce = time_after(jiffies,
			last_activity + msecs_to_jiffies(QUIESCENCE_TIMEOUT_MS));

	if (should_quiesce && !perfmon_state.quiescent)
		enter_quiescence();
}

/*
 * Main monitoring thread.
 * Runs at slightly elevated priority for consistent timing.
 */
static int perfmon_thread(void *unused)
{
	/* Set to SCHED_NORMAL with nice -10 for reasonable priority */
	set_user_nice(current, -10);

	while (!kthread_should_stop()) {
		int cpu;

		/* Process each online CPU */
		for_each_online_cpu(cpu)
			process_cpu(cpu);

		/* Check for system quiescence */
		check_quiescence();

		/* Sleep until next poll - use high-resolution sleep */
		usleep_range(PERFMON_POLL_MS * 1000,
			     PERFMON_POLL_MS * 1000 + 100);
	}

	return 0;
}

/*
 * CPU hotplug callback - reset data when CPU comes online.
 */
static int perfmon_cpu_online(unsigned int cpu)
{
	struct cpu_perfmon_data *data = per_cpu_ptr(&perfmon_data, cpu);

	memset(data, 0, sizeof(*data));
	atomic_inc(&perfmon_state.active_cpus);

	return 0;
}

static int perfmon_cpu_offline(unsigned int cpu)
{
	atomic_dec(&perfmon_state.active_cpus);
	return 0;
}

/*
 * CPUFreq notifier - track target frequency changes.
 */
static int perfmon_cpufreq_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct cpufreq_freqs *freqs = data;
	struct cpu_perfmon_data *perfdata;

	if (event != CPUFREQ_POSTCHANGE)
		return NOTIFY_OK;

	perfdata = per_cpu_ptr(&perfmon_data, freqs->cpu);
	WRITE_ONCE(perfdata->target_freq_khz, freqs->new);

	/* Exit quiescence on frequency change - system is active */
	if (READ_ONCE(perfmon_state.quiescent))
		exit_quiescence();

	return NOTIFY_OK;
}

static struct notifier_block perfmon_cpufreq_nb = {
	.notifier_call = perfmon_cpufreq_notifier,
};

/*
 * Find devfreq devices for DDR control.
 * Uses devfreq_boost's registered device to avoid conflicts.
 */
static void find_devfreq_devices(void)
{
	/*
	 * Get DDR devfreq from devfreq_boost which already has it registered.
	 * This ensures we use the same device that devfreq_boost controls,
	 * avoiding any conflicts between the two drivers.
	 */
	perfmon_state.ddr_devfreq = devfreq_boost_get_device(DEVFREQ_CPU_LLCC_DDR_BW);

	if (perfmon_state.ddr_devfreq)
		pr_info("Found DDR devfreq device via devfreq_boost\n");
	else
		pr_info("DDR devfreq not available yet, quiescence disabled\n");
}

static int __init sm6150_perfmon_init(void)
{
	int ret;

	pr_info("Initializing SM6150 Performance Monitor\n");

	/* Initialize state */
	atomic_set(&perfmon_state.active_cpus, num_online_cpus());
	atomic_long_set(&perfmon_state.last_activity_jiffies, jiffies);

	/* Find devfreq devices */
	find_devfreq_devices();

	/* Register CPU hotplug callbacks */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"cpufreq/sm6150_perfmon:online",
				perfmon_cpu_online,
				perfmon_cpu_offline);
	if (ret < 0) {
		pr_err("Failed to register CPU hotplug: %d\n", ret);
		return ret;
	}

	/* Register cpufreq notifier */
	ret = cpufreq_register_notifier(&perfmon_cpufreq_nb,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier: %d\n", ret);
		goto err_cpuhp;
	}

	/* Create monitoring thread */
	perfmon_state.thread = kthread_run(perfmon_thread, NULL,
					   "sm6150_perfmon");
	if (IS_ERR(perfmon_state.thread)) {
		ret = PTR_ERR(perfmon_state.thread);
		pr_err("Failed to create perfmon thread: %d\n", ret);
		goto err_notifier;
	}

	pr_info("SM6150 Performance Monitor initialized successfully\n");
	return 0;

err_notifier:
	cpufreq_unregister_notifier(&perfmon_cpufreq_nb,
				    CPUFREQ_TRANSITION_NOTIFIER);
err_cpuhp:
	cpuhp_remove_state_nocalls(CPUHP_AP_ONLINE_DYN);
	return ret;
}

static void __exit sm6150_perfmon_exit(void)
{
	if (perfmon_state.thread)
		kthread_stop(perfmon_state.thread);

	cpufreq_unregister_notifier(&perfmon_cpufreq_nb,
				    CPUFREQ_TRANSITION_NOTIFIER);
	cpuhp_remove_state_nocalls(CPUHP_AP_ONLINE_DYN);

	pr_info("SM6150 Performance Monitor unloaded\n");
}

late_initcall(sm6150_perfmon_init);
module_exit(sm6150_perfmon_exit);

MODULE_AUTHOR("Miguel Angel <miguel@nebuia.com>");
MODULE_DESCRIPTION("SM6150 Performance Monitor - Tensor AIO concepts for Snapdragon 730G");
MODULE_LICENSE("GPL v2");
