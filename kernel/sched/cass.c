// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Adapted for kernel 4.14 with WALT by kernel developer, 2025.
 *
 * Optimizations for ARM big.LITTLE and improved compatibility.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 *
 * This version is adapted for kernel 4.14 with Qualcomm WALT scheduler
 * extensions and optimized for ARM big.LITTLE architectures.
 */

/*
 * The margin used when comparing utilization with CPU capacity.
 * (default: ~20%) - capacity * 1.25 must fit
 */
#define fits_capacity(cap, max)	((cap) * 1280 < (max) * 1024)

/*
 * Compatibility layer for kernel 4.14
 * These functions provide equivalent functionality to newer kernels
 */

/* Get RT task utilization - uses rt_avg scaled to capacity */
static __always_inline unsigned long cass_cpu_util_rt(struct rq *rq)
{
#ifdef CONFIG_SCHED_WALT
	/* WALT tracks RT time differently */
	return (rq->rt_avg * arch_scale_cpu_capacity(NULL, cpu_of(rq)))
		>> SCHED_CAPACITY_SHIFT;
#else
	struct rt_rq *rt_rq = &rq->rt;
	return rt_rq->avg.util_avg;
#endif
}

/* Get DL task utilization - use running_bw scaled to capacity */
static __always_inline unsigned long cass_cpu_util_dl(struct rq *rq)
{
	/*
	 * Kernel 4.14 doesn't have per-entity DL load tracking.
	 * Use running_bw as approximation, scaled to capacity units.
	 * running_bw is in 2^BW_SHIFT units, convert to capacity scale.
	 */
	return (rq->dl.running_bw * arch_scale_cpu_capacity(NULL, cpu_of(rq)))
		>> BW_SHIFT;
}

/* Get IRQ utilization */
static __always_inline unsigned long cass_cpu_util_irq(struct rq *rq)
{
#ifdef CONFIG_SCHED_WALT
	/* WALT tracks IRQ load - scale to capacity */
	return (rq->avg_irqload * arch_scale_cpu_capacity(NULL, cpu_of(rq)))
		>> SCHED_CAPACITY_SHIFT;
#else
	return 0;
#endif
}

/* Check if CPU is available and idle */
static __always_inline bool cass_available_idle_cpu(int cpu)
{
	if (!idle_cpu(cpu))
		return false;

#ifdef CONFIG_HOTPLUG_CPU
	if (unlikely(!cpu_active(cpu)))
		return false;
#endif
	return true;
}

/*
 * Check if CPU only has very low priority tasks - simplified for 4.14.
 * Kernel 4.14 doesn't track SCHED_IDLE tasks separately, so we approximate
 * by checking if only CFS tasks with very low weight are running.
 */
static __always_inline bool cass_sched_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/* If completely idle, not a sched_idle scenario */
	if (idle_cpu(cpu))
		return false;

	/*
	 * Approximate: if only 1 CFS task running with very low load,
	 * treat as "idle-ish". This is a simplification since 4.14
	 * doesn't have proper SCHED_IDLE tracking.
	 */
	return rq->nr_running == 1 && rq->cfs.h_nr_running == 1 &&
	       rq->cfs.avg.util_avg < (SCHED_CAPACITY_SCALE >> 4);
}

/* Get thermal pressure - approximated from capacity reduction */
static __always_inline unsigned long cass_thermal_load_avg(struct rq *rq)
{
	unsigned long cap_orig = arch_scale_cpu_capacity(NULL, cpu_of(rq));
	unsigned long cap_curr = rq->cpu_capacity;

	/* Thermal pressure is the difference between original and current */
	return (cap_orig > cap_curr) ? (cap_orig - cap_curr) : 0;
}

struct cass_cpu_cand {
	unsigned long cap;
	unsigned long cap_max;
	unsigned long cap_orig;
	unsigned long eff_util;
	unsigned long hard_util;
	unsigned long util;
	unsigned int exit_lat;
	int cpu;
	/* Pack booleans together for cache efficiency */
	u8 is_idle;
};

static __always_inline
void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long util_avg;
#ifdef CONFIG_SCHED_WALT
	unsigned long walt_util;
#endif

	/* Get this CPU's utilization from CFS tasks */
	util_avg = READ_ONCE(cfs_rq->avg.util_avg);

#ifdef CONFIG_SCHED_WALT
	/* Use WALT utilization if available and higher */
	if (likely(sysctl_sched_use_walt_cpu_util)) {
		walt_util = cpu_util_cum(c->cpu, 0);
		if (walt_util > util_avg)
			util_avg = walt_util;
	}
#endif

	/* Use util_est if available and higher */
	if (sched_feat(UTIL_EST)) {
		unsigned long est = READ_ONCE(cfs_rq->avg.util_est.enqueued);
		if (est > util_avg) {
			sync = false;
			util_avg = est;
		}
	}

	c->util = util_avg;

	/*
	 * Deduct @current's util from this CPU if this is a sync wake, unless
	 * @current is an RT task; RT tasks don't have per-entity load tracking.
	 */
	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, task_util(current));

	/* Get the utilization of everything other than CFS tasks */
	c->hard_util = cass_cpu_util_rt(rq) + cass_cpu_util_dl(rq) +
		       cass_cpu_util_irq(rq);

	/*
	 * Account for lost capacity due to time spent in RT/DL tasks and IRQs.
	 */
	c->cap = c->cap_max - min(c->hard_util, c->cap_max - 1);
}

/*
 * Returns true if @c is a CPU with the maximum possible original capacity and
 * there's only one such CPU in the system (i.e., if @c is the prime CPU).
 * On SM6150, this would be CPU7 (highest performance core).
 */
static __always_inline
bool cass_prime_cpu(const struct cass_cpu_cand *c)
{
	/*
	 * On arm64 big.LITTLE, the prime CPU is typically the last CPU.
	 * Check if it has different capacity than the previous one.
	 */
	if (unlikely(c->cpu >= nr_cpu_ids - 1))
		return false;

	return c->cpu == nr_cpu_ids - 1 &&
	       arch_scale_cpu_capacity(NULL, nr_cpu_ids - 2) !=
	       arch_scale_cpu_capacity(NULL, nr_cpu_ids - 1);
}

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     int this_cpu, int prev_cpu, bool sync)
{
#define cass_cmp(a, b) ({ res = (long)(a) - (long)(b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU that's not overloaded */
	if (cass_cmp(b->eff_util / b->cap_max, a->eff_util / a->cap_max))
		goto done;

	/* Prefer the CPU that's less overloaded if they're both overloaded */
	if (b->eff_util > b->cap_max && a->eff_util > a->cap_max &&
	    cass_cmp(b->eff_util * SCHED_CAPACITY_SCALE / b->cap_max,
		     a->eff_util * SCHED_CAPACITY_SCALE / a->cap_max))
		goto done;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(fits_capacity(p_util, a->cap_max),
		     fits_capacity(p_util, b->cap_max)))
		goto done;

	/* Prefer the CPU that isn't the single fastest one in the system */
	if (cass_cmp(cass_prime_cpu(b), cass_prime_cpu(a)))
		goto done;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the CPU that is idle */
	if (cass_cmp(a->is_idle, b->is_idle))
		goto done;

	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the previous CPU (cache hot) */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/* Prefer the CPU that shares a cache with the previous CPU */
	if (cass_cmp(cpus_share_cache(a->cpu, prev_cpu),
		     cpus_share_cache(b->cpu, prev_cpu)))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
#undef cass_cmp
#undef cass_eq
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync, bool rt)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands;
	int this_cpu = raw_smp_processor_id();
	unsigned long p_util;
	bool has_idle = false;
	int cidx = 0, cpu;

	/*
	 * Get the utilization for this task. Note that RT tasks don't have
	 * per-entity load tracking in kernel 4.14.
	 */
	p_util = rt ? 0 : task_util_est(p);

	/*
	 * Find the best CPU to wake @p on.
	 * Note: @curr->cpu must be initialized before this loop ends.
	 */
	for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
		/* Use the free candidate slot for @curr */
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);

		/* Get the original, maximum _possible_ capacity of this CPU */
		curr->cap_orig = arch_scale_cpu_capacity(NULL, cpu);

		/* Get the _current_, throttled maximum capacity of this CPU */
		curr->cap_max = curr->cap_orig - cass_thermal_load_avg(rq);

		/* Ensure cap_max is at least 1 to avoid division by zero */
		if (unlikely(curr->cap_max == 0))
			curr->cap_max = 1;

		curr->cpu = cpu;
		curr->is_idle = 0;

		/*
		 * Check if this CPU is idle or only has SCHED_IDLE tasks.
		 * For sync wakes, treat the current CPU as idle if @current
		 * is the only running task.
		 */
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    cass_available_idle_cpu(cpu) || cass_sched_idle_cpu(cpu)) {
			/*
			 * A non-idle candidate may be better for energy
			 * efficiency when the only idle candidate found so far
			 * is the prime CPU. Otherwise, prefer idle candidates.
			 */
			if (!cass_prime_cpu(curr)) {
				/* Discard any previous non-idle candidate */
				if (!has_idle)
					best = curr;
				has_idle = true;
			}

			curr->is_idle = 1;

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;

			/* Add on the actual idle exit latency, if any */
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle)
				continue;

			/* Zero exit latency indicates this CPU isn't idle */
			curr->exit_lat = 0;
		}

		/* Get this CPU's capacity and utilization */
		cass_cpu_util(curr, this_cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like if
		 * @p were on it.
		 */
		if (cpu != task_cpu(p))
			curr->util += p_util;

		/*
		 * Calculate the effective utilization for this CPU candidate;
		 * i.e., the utilization calculated by the CPU governor.
		 */
		curr->eff_util = curr->util + curr->hard_util;

		/*
		 * Calculate the relative utilization for this CPU candidate.
		 * Use cap_orig to fairly distribute load regardless of thermal
		 * throttling.
		 */
		if (likely(curr->cap_orig))
			curr->util = curr->util * SCHED_CAPACITY_SCALE /
				     curr->cap_orig;

		/*
		 * Check if this CPU is better than the best CPU found so far.
		 * If @best == @curr then there's no need to compare them, but
		 * cidx still needs to be changed to the other candidate slot.
		 */
		if (best == curr ||
		    cass_cpu_better(curr, best, p_util, this_cpu, prev_cpu,
				    sync)) {
			best = curr;
			cidx ^= 1;
		}
	}

	return best->cpu;
}

static int cass_select_task_rq(struct task_struct *p, int prev_cpu,
			       int wake_flags, bool rt)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (wake_flags & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(&p->cpus_allowed, cpu_active_mask)))
		return cpumask_first(&p->cpus_allowed);

	/* cass_best_cpu() needs the CFS task's utilization, so sync it up */
	if (!rt && !(wake_flags & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync, rt);
}

/*
 * Kernel 4.14 select_task_rq signature:
 * (struct task_struct *p, int cpu, int sd_flag, int wake_flags, int sibling_count_hint)
 */
static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags,
				    int sibling_count_hint __always_unused)
{
	/*
	 * Combine sd_flag and wake_flags for our internal function.
	 * SD_BALANCE_EXEC/FORK are in sd_flag, WF_SYNC is in wake_flags.
	 */
	return cass_select_task_rq(p, prev_cpu, sd_flag | wake_flags, false);
}

int cass_select_task_rq_rt(struct task_struct *p, int prev_cpu,
			   int sd_flag, int wake_flags,
			   int sibling_count_hint __always_unused)
{
	return cass_select_task_rq(p, prev_cpu, sd_flag | wake_flags, true);
}
