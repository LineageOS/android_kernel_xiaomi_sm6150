// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Adapted for kernel 4.14 by kernel developer, 2025.
 */

#ifdef CONFIG_SCHED_CASS
/*
 * Kernel 4.14 select_task_rq signature has 5 parameters.
 */
int cass_select_task_rq_rt(struct task_struct *p, int prev_cpu,
			   int sd_flag, int wake_flags,
			   int sibling_count_hint);

/* Use CASS. A dummy wrapper ensures the replaced function is still "used". */
static inline void *select_task_rq_rt_dummy(void)
{
	return (void *)select_task_rq_rt;
}
#define select_task_rq_rt cass_select_task_rq_rt
#endif /* CONFIG_SCHED_CASS */
