// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Screen-off max freq limiting added for power optimization.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/msm_drm_notify.h>
#include <linux/slab.h>
#include <linux/version.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

/*
 * Screen-off power optimization: Limit max frequency when display is off.
 * This prevents the CPU from ramping up due to background tasks, wakelocks,
 * or predictive load algorithms when the screen is off.
 */
#define SCREEN_OFF_LIMIT_DELAY_MS	500

/*
 * Screen-off max frequencies - using actual OPP table values.
 * LP cluster:   300000, 576000, 768000, 1017600...
 * Perf cluster: 300000, 652800, 806400, 979200...
 */
#define SCREEN_OFF_MAX_FREQ_LP		768000U		/* 768 MHz - low power */
#define SCREEN_OFF_MAX_FREQ_PERF	979200U		/* 979 MHz - ~44% of max */

/*
 * State bits - ordered by frequency of access for better branch prediction.
 * SCREEN_OFF is checked most frequently in hot paths.
 */
enum {
	SCREEN_OFF = 0,
	SCREEN_OFF_LIMIT_ACTIVE,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct delayed_work screen_off_limit;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long state;
};

static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);
static void screen_off_limit_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
						  max_unboost_worker, 0),
	.screen_off_limit = __DELAYED_WORK_INITIALIZER(boost_drv_g.screen_off_limit,
						       screen_off_limit_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

static unsigned int get_input_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = CONFIG_INPUT_BOOST_FREQ_LP;
	else
		freq = CONFIG_INPUT_BOOST_FREQ_PERF;

	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = CONFIG_MAX_BOOST_FREQ_LP;
	else
		freq = CONFIG_MAX_BOOST_FREQ_PERF;

	return min(freq, policy->max);
}

/*
 * Get the maximum frequency allowed during screen-off state.
 * This caps how high the CPU can go when display is off to save power.
 * Inlined for hot path performance - called from notifier callback.
 */
static __always_inline unsigned int get_screen_off_max_freq(
		struct cpufreq_policy *policy)
{
	const unsigned int freq = cpumask_test_cpu(policy->cpu, cpu_lp_mask) ?
				  SCREEN_OFF_MAX_FREQ_LP : SCREEN_OFF_MAX_FREQ_PERF;

	/* Ensure we don't go below min or above actual max */
	return clamp(freq, policy->cpuinfo.min_freq, policy->cpuinfo.max_freq);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	if (test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
			      msecs_to_jiffies(CONFIG_INPUT_BOOST_DURATION_MS)))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick(b);
}
EXPORT_SYMBOL(cpu_input_boost_kick);

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick_max(b, duration_ms);
}
EXPORT_SYMBOL(cpu_input_boost_kick_max);

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

/*
 * Screen-off frequency limit worker: After a delay, activate aggressive
 * frequency limiting to ensure CPU reaches minimum frequencies during
 * extended screen-off periods (sleep, pocket, etc.)
 */
static void screen_off_limit_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), screen_off_limit);

	/* Only apply if still in screen-off state (likely since we just woke) */
	if (likely(test_bit(SCREEN_OFF, &b->state))) {
		set_bit(SCREEN_OFF_LIMIT_ACTIVE, &b->state);
		wake_up(&b->boost_waitq);
	}
}

static int cpu_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	/* Run on performance cores for lowest latency */
	set_cpus_allowed_ptr(current, cpu_perf_mask);
	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	unsigned long state;

	if (unlikely(action != CPUFREQ_ADJUST))
		return NOTIFY_OK;

	/* Cache state to avoid multiple volatile reads */
	state = READ_ONCE(b->state);

	/* Screen-off power optimization: limit both min AND max frequency */
	if (state & BIT(SCREEN_OFF)) {
		policy->min = policy->cpuinfo.min_freq;

		/*
		 * After delay, also limit max frequency to prevent CPU from
		 * ramping up due to background tasks or predictive algorithms.
		 * This is the key fix for the 576MHz overnight issue.
		 */
		if (state & BIT(SCREEN_OFF_LIMIT_ACTIVE))
			policy->max = get_screen_off_max_freq(policy);

		return NOTIFY_OK;
	}

	/* Boost CPU to max frequency for max boost */
	if (unlikely(state & BIT(MAX_BOOST))) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & BIT(INPUT_BOOST))
		policy->min = get_input_boost_freq(policy);
	else
		policy->min = policy->cpuinfo.min_freq;

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	int *blank = ((struct msm_drm_notifier *)data)->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		/* Screen ON: Cancel screen-off limit and restore full range */
		cancel_delayed_work_sync(&b->screen_off_limit);
		clear_bit(SCREEN_OFF_LIMIT_ACTIVE, &b->state);
		clear_bit(SCREEN_OFF, &b->state);
		__cpu_input_boost_kick_max(b, CONFIG_WAKE_BOOST_DURATION_MS);
	} else {
		/* Screen OFF: Set state and schedule aggressive limiting */
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);

		/*
		 * Schedule delayed work to activate max freq limiting.
		 * The delay allows brief wakeups (notifications, alarms)
		 * to complete at normal frequencies before limiting kicks in.
		 */
		mod_delayed_work(system_unbound_wq, &b->screen_off_limit,
				 msecs_to_jiffies(SCREEN_OFF_LIMIT_DELAY_MS));
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

unregister_fb_notif:
	msm_drm_unregister_client(&b->msm_drm_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
