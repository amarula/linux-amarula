#ifndef _LINUX_PSI_TYPES_H
#define _LINUX_PSI_TYPES_H

#include <linux/types.h>

#ifdef CONFIG_PSI

/* Tracked task states */
enum psi_task_count {
	NR_RUNNING,
	NR_IOWAIT,
	NR_MEMSTALL,
	NR_PSI_TASK_COUNTS,
};

/* Task state bitmasks */
#define TSK_RUNNING	(1 << NR_RUNNING)
#define TSK_IOWAIT	(1 << NR_IOWAIT)
#define TSK_MEMSTALL	(1 << NR_MEMSTALL)

/* Resources that workloads could be stalled on */
enum psi_res {
	PSI_CPU,
	PSI_MEM,
	PSI_IO,
	NR_PSI_RESOURCES,
};

/* Pressure states for a group of tasks */
enum psi_state {
	PSI_NONE,		/* No stalled tasks */
	PSI_SOME,		/* Stalled tasks & working tasks */
	PSI_FULL,		/* Stalled tasks & no working tasks */
	NR_PSI_STATES,
};

struct psi_resource {
	/* Current pressure state for this resource */
	enum psi_state state;

	/* Start of current state (rq_clock) */
	u64 state_start;

	/* Time sampling buckets for pressure states SOME and FULL (ns) */
	u64 times[2];
};

struct psi_group_cpu {
	/* States of the tasks belonging to this group */
	unsigned int tasks[NR_PSI_TASK_COUNTS];

	/* There are runnable or D-state tasks */
	int nonidle;

	/* Start of current non-idle state (rq_clock) */
	u64 nonidle_start;

	/* Time sampling bucket for non-idle state (ns) */
	u64 nonidle_time;

	/* Per-resource pressure tracking in this group */
	struct psi_resource res[NR_PSI_RESOURCES];
};

struct psi_group {
	struct psi_group_cpu *cpus;

	struct mutex stat_lock;

	u64 some[NR_PSI_RESOURCES];
	u64 full[NR_PSI_RESOURCES];

	unsigned long period_expires;

	u64 last_some[NR_PSI_RESOURCES];
	u64 last_full[NR_PSI_RESOURCES];

	unsigned long avg_some[NR_PSI_RESOURCES][3];
	unsigned long avg_full[NR_PSI_RESOURCES][3];

	struct delayed_work clock_work;
};

#else /* CONFIG_PSI */

struct psi_group { };

#endif /* CONFIG_PSI */

#endif /* _LINUX_PSI_TYPES_H */
