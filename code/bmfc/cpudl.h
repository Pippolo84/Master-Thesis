/*
 *  kernel/sched/cpudl.h
 *
 *  CPU deadlines global management
 *
 *  Author: Fabio Falzoi <fabio.falzoi@alice.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/types.h>

#include "bm_fc.h"

#define CPUDL_MAX_LEVEL		8
#define NOT_IN_LIST		-1
#define CPUDL_RAND_MAX		~0UL
#define CPUDL_HEAD_IDX		-1
#define NO_CACHED_CPU		-1

struct skiplist_item {
	u64 dl;
	int level;
	struct skiplist_item *next[CPUDL_MAX_LEVEL];
	struct skiplist_item *prev[CPUDL_MAX_LEVEL];
	int cpu;
};

struct cpudl {
	struct skiplist_item *head;
	struct skiplist_item *cpu_to_item[NR_CPUS];
	unsigned int level;

	cpumask_var_t free_cpus;

	bool (*cmp_dl)(u64 a, u64 b);

	struct flat_combining *fc;
};

#ifdef CONFIG_SMP
int cpudl_find(struct cpudl *cp, struct cpumask *dlo_mask,
		struct task_struct *p, struct cpumask *later_mask);
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid);
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b));
void cpudl_cleanup(struct cpudl *cp);
#else
#define cpudl_set(cp, cpu, dl) do { } while (0)
#define cpudl_init() do { } while (0)
#endif /* CONFIG_SMP */

#endif /* _LINUX_CPUDL_H */
