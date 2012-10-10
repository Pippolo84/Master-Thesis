/*
 *  kernel/sched_cpudl.c
 *
 *  Global CPU deadlines management
 *
 *  Author: Fabio Falzoi <falzoi@tecip.sssup.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/atomic.h>
#include <linux/atomic.h>
#include <asm/barrier.h>

#include "bm_fc.h"
#include "cpudl.h"

static inline u64 cpudl_detach(struct cpudl *list, struct skiplist_item *p)
{
	int i;

	for(i = 0; i <= p->level; i++) {
		p->prev[i]->next[i] = p->next[i];
		if(p->next[i])
			p->next[i]->prev[i] = p->prev[i];
	}

	while(!list->head->next[list->level] && list->level > 0)
		list->level--;

	p->level = NOT_IN_LIST;

	return p->dl;
}

static u64 cpudl_remove_idx(struct cpudl *list, const int cpu)
{
	struct skiplist_item *p;

	p = list->cpu_to_item[cpu];

	if(p->level == NOT_IN_LIST)
		return 0;

	cpumask_set_cpu(cpu, list->free_cpus);

	return cpudl_detach(list, p);
}

static inline unsigned int cpudl_rand_level(unsigned int max)
{
	unsigned int level = 0, sorted;
	struct timespec limit;

	max = max > CPUDL_MAX_LEVEL - 1 ? CPUDL_MAX_LEVEL - 1 : max;

	do {
		level++;
		limit = current_kernel_time();
		sorted = ((unsigned int)limit.tv_nsec % CPUDL_RAND_MAX);
	} while((sorted >= (((float)(1 - LEVEL_PROB_VALUE)) * CPUDL_RAND_MAX)) && level < max);

	return level;
}

static int cpudl_insert(struct cpudl *list, const int cpu, u64 dl)
{
	struct skiplist_item *p;
	struct skiplist_item *update[CPUDL_MAX_LEVEL];
	struct skiplist_item *new_node;
	int cmp_res, level, i;
	unsigned int rand_level;

	new_node = list->cpu_to_item[cpu];

	new_node->dl = dl;

	p = list->head;
	level = list->level;
	while(level >= 0) {
		update[level] = p;

		if(!p->next[level]) {
			level--;
			continue;
		}

		cmp_res = list->cmp_dl(new_node->dl, p->next[level]->dl);

		if(cmp_res > 0)
			p = p->next[level];
		else
			level--;
	}

	rand_level = cpudl_rand_level(list->level + 1);
	new_node->level = rand_level;

	if(rand_level > list->level)
		update[++list->level] = list->head;

	for(i = 0; i <= rand_level; i++) {
		new_node->next[i] = update[i]->next[i];
		update[i]->next[i] = new_node;
		new_node->prev[i] = update[i];
		if(new_node->next[i])
			new_node->next[i]->prev[i] = new_node;
	}

	cpumask_clear_cpu(cpu, list->free_cpus);

	return 0;
}

static void cpudl_dispatcher(void *list, int cpu, u64 dline, int is_valid)
{
	struct cpudl *cp = (struct cpudl *)list;
	
	cpudl_remove_idx(cp, cpu);

	if(is_valid)
		cpudl_insert(cp, cpu, dline);
}

/*
 * cpudl_find - find the best (later-dl) CPU in the system
 * @cp: the cpudl skiplist context
 * @dlo_mask: mask of overloaded runqueues in the root domain (not used)
 * @p: the task
 * @later_mask: a mask to fill in with the selected CPUs (or NULL)
 *
 * Returns: int - best CPU (skiplist maximum if suitable)
 */
int cpudl_find(struct cpudl *cp, struct cpumask *dlo_mask,
		struct task_struct *p, struct cpumask *later_mask)
{
	u64 first_dl;
	int first_cpu, best_cpu = -1;
	const struct sched_dl_entity *dl_se;

	/*
	 * for push operation, first we 
	 * search a suitable cpu in 
	 * cp->free_cpus (free CPUs mask),
	 * otherwise we ask a cpu index
	 * from cpudl
	 */
	if(p)
		dl_se = &p->dl;
	if(later_mask && cpumask_and(later_mask, cp->free_cpus,
			&p->cpus_allowed) && cpumask_and(later_mask,
			later_mask, cpu_active_mask)) {
		best_cpu = cpumask_any(later_mask);
	} else {
		/*
		 * we read best cpu from
		 * flat combining cache
		 */
		first_cpu = atomic_read(&cp->fc->cached_cpu);
		if(first_cpu < 0)
			return -1;
		else
			first_dl = (u64)atomic64_read(&cp->fc->current_dl[first_cpu]);
		/*
		 * if cpudl_find is called on behalf of
		 * a pull attempt, or first_cpu is equal
		 * to -1, we can not do anything, 
		 * so we return immediately
		 * the CPU value from cpudl structure
		 */
		if(!p)
			return first_cpu;
		
		/*
		 * if cpudl_find is called for
		 * a push we must check the cpus_allowed
		 * mask and the deadline
		 */
		if(cpumask_test_cpu(first_cpu, &p->cpus_allowed) && 
			cp->cmp_dl(dl_se->deadline, first_dl)) {
				best_cpu = first_cpu;
				if(later_mask)
					cpumask_set_cpu(best_cpu, later_mask);
		}
	}

	return best_cpu;
}

/*
 * cpudl_set - update the cpudl skiplist
 * @cp: the cpudl skiplist context
 * @cpu: the target cpu
 * @dl: the new earliest deadline for this cpu
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid)
{
	struct pub_record *rec;
	int now_cached_cpu;
	u64 now_cached_dl = 0;

	/*
	 * if is_valid is set we may have
	 * to update the cached CPU
	 */
	if(is_valid) {
		/* we update immediately our deadline */
		atomic64_set(&cp->fc->current_dl[cpu], dl);
		while(1) {
			now_cached_cpu = atomic_read(&cp->fc->cached_cpu);
			if(now_cached_cpu != NO_CACHED_CPU)
				now_cached_dl = (u64)atomic64_read(&cp->fc->current_dl[now_cached_cpu]);
			/* 
			 * check if we have to update cached CPU value 
			 * we break the loop if the value must not be
			 * updated or if we have to and the update succeed
			 */
			if((now_cached_cpu != NO_CACHED_CPU &&
				now_cached_cpu != cpu &&
				cp->cmp_dl(now_cached_dl, dl)) ||
				atomic_cmpxchg(&cp->fc->cached_cpu, now_cached_cpu, cpu) == cpu)
				break;
		}
	}
	
	/*
	 * if is_valid is clear we may have
	 * to clear the cached CPU
	 */
	if(!is_valid) {
		/* we update immediately our deadline */
		atomic64_set(&cp->fc->current_dl[cpu], 0);
		while(1) {
			now_cached_cpu = atomic_read(&cp->fc->cached_cpu);
			if(now_cached_cpu != NO_CACHED_CPU)
				now_cached_dl = (u64)atomic64_read(&cp->fc->current_dl[now_cached_cpu]);
			if((now_cached_cpu != NO_CACHED_CPU && now_cached_cpu != cpu) ||
				atomic_cmpxchg(&cp->fc->cached_cpu, now_cached_cpu, cpu) == cpu)
				break;
		}
	}

	rec = fc_get_record(cp->fc, cpu);
	rec->req = SET;
	rec->par.set_p.cpu = cpu;
	rec->par.set_p.dline = dl;
	rec->par.set_p.is_valid = is_valid;
	rec->h.set_h.function = cpudl_dispatcher;
	fc_publish_record(cp->fc, cpu);

	fc_try_combiner(cp->fc);
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl skiplist context
 * @cmp_dl: function used to order deadlines inside structure
 */
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b))
{
	int i;

	memset(cp, 0, sizeof(*cp));
	cp->cmp_dl = cmp_dl;

	cp->fc = fc_create(cp);
	atomic_set(&cp->fc->cached_cpu, NO_CACHED_CPU);

	cp->head = (struct skiplist_item *)kmalloc(sizeof(*cp->head), GFP_KERNEL);
	memset(cp->head, 0, sizeof(*cp->head));
	cp->head->cpu = CPUDL_HEAD_IDX;

	memset(cp->cpu_to_item, 0, sizeof(*cp->cpu_to_item) * NR_CPUS);
	for(i = 0; i < NR_CPUS; i++) {
		cp->cpu_to_item[i] = (struct skiplist_item *)kmalloc(sizeof(*cp->cpu_to_item[i]), GFP_KERNEL);
		memset(cp->cpu_to_item[i], 0, sizeof(*cp->cpu_to_item[i]));
		cp->cpu_to_item[i]->level = NOT_IN_LIST;
		cp->cpu_to_item[i]->cpu = i;
	}

	if(!alloc_cpumask_var(&cp->free_cpus, GFP_KERNEL))
		return -ENOMEM;
	cpumask_setall(cp->free_cpus);
	
	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl skiplist context
 */
void cpudl_cleanup(struct cpudl *cp)
{
	int i;	

	for(i = 0; i < NR_CPUS; i++)
		kfree(cp->cpu_to_item[i]);
	kfree(cp->head);

	fc_destroy(cp->fc);
}
