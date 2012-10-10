/*
 *  kernel/sched/bm_fc.h
 *
 *  Bitmap Flat Combining source file
 *
 *  Author: Fabio Falzoi <fabio.falzoi@alice.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/threads.h>
#include <linux/bitops.h>
#include <linux/atomic.h>
#include <asm/barrier.h>

#include "bm_fc.h"

/* bitmap management helper functions */
inline void bitmap64_set(u64 *bitmap, int n)
{
	*bitmap |= ((u64)1 << n);
	smp_wmb();
}

inline void bitmap32_set(u32 *bitmap, int n)
{
	*bitmap |= ((u32)1 << n);
	smp_wmb();
}

inline void bitmap64_clear(u64 *bitmap, int n)
{
	*bitmap &= ~((u64)1 << n);
	smp_wmb();
}

inline void bitmap32_clear(u32 *bitmap, int n)
{
	*bitmap &= ~((u32)1 << n);
	smp_wmb();
}

inline int bitmap64_test(u64 *bitmap, int n)
{
	smp_rmb();
	return ((*bitmap & ((u64)1 << n)) > (u64)0);
}

inline int bitmap32_test(u32 *bitmap, int n)
{
	smp_rmb();
	return ((*bitmap & ((u32)1 << n)) > (u32)0);
}

inline int bitmap64_fls(u64 *bitmap)
{
	smp_rmb();
	return fls64(*bitmap) - 1;
}

inline int bitmap32_fls(u32 *bitmap)
{
	smp_rmb();
	return fls(*bitmap) - 1;
}

/* data structure lock interface */
void fc_lock(struct data_structure_lock *ds_lock)
{
	int old, ret;

	while(1) {
		smp_rmb();
		old = atomic_read(&ds_lock->lock);
		if(old == DS_LOCKED)
			continue;
		/*
		 * Any atomic operation that modifies 
		 * some state in memory and returns information
		 * about the state implies an SMP-conditional 
		 * general memory barrier on each side of the 
		 * actual operation
		 *
		 * See Documentation/memory-barriers.txt for
		 * further details
		 */
		ret = atomic_cmpxchg(&ds_lock->lock, old, DS_LOCKED);
		if(ret == old)
			break;
	}
}

int fc_trylock(struct data_structure_lock *ds_lock)
{
	int old, ret;

	smp_rmb();
	old = atomic_read(&ds_lock->lock);
	if(old == DS_LOCKED)
		return -1;
	ret = atomic_cmpxchg(&ds_lock->lock, old, DS_LOCKED);
	if(ret == old)
		return 0;
	else
		return -1;
}

void fc_unlock(struct data_structure_lock *ds_lock)
{
	/*
	 * Since atomic_set() doesn't returns
	 * anything about new or old memory state
	 * we have to issue a memory barrier
	 */
	atomic_set(&ds_lock->lock, DS_UNLOCKED);
	smp_wmb();
}

/* flat combining interface */
static void fc_do_combiner(struct flat_combining *fc)
{
	struct pub_list *map = &fc->map;
	struct pub_record *rec;
	int cpu_index, rec_index;

	while((cpu_index = bitmap64_fls(&map->cpu_bitmap)) >= 0) {
		while((rec_index = bitmap32_fls(&map->rec_bitmap[cpu_index])) >= 0) {
			rec = &map->rec_array[cpu_index * PUB_RECORD_PER_CPU + rec_index];
			switch(rec->req) {
				case SET:
					rec->h.set_h.function(fc->data_structure,
								rec->par.set_p.cpu,
								rec->par.set_p.dline,
								rec->par.set_p.is_valid);
					break;
				default:
					printk(KERN_ERR "ERROR: unknown operation type on cpu %d pub record\n", cpu_index);
			}
			bitmap32_clear(&map->rec_bitmap[cpu_index], rec_index);
		}
		bitmap64_clear(&map->cpu_bitmap, cpu_index);
	}
}

struct flat_combining *fc_create(void *data_structure)
{
	struct flat_combining *fc;

	fc = (struct flat_combining *)kmalloc(sizeof(*fc), GFP_KERNEL);
	memset(fc, 0, sizeof(*fc));
	atomic_set(&fc->ds_lock.lock, DS_UNLOCKED);
	smp_wmb();
	fc->data_structure = data_structure;

	return fc;
}

int fc_destroy(struct flat_combining *fc)
{
	if(fc) {
		kfree(fc);
		return 0;
	}

	return -1;
}

struct pub_record *fc_get_record(struct flat_combining *fc, const int cpu)
{
	struct pub_list *map = &fc->map;
	int idx_to_use;
	
	/* next publication record to use */
	idx_to_use = map->last_used_idx[cpu];
	
	while(1) {
		/* if record is not busy we use it */
		if(!bitmap32_test(&map->rec_bitmap[cpu], idx_to_use))
			return &map->rec_array[cpu * PUB_RECORD_PER_CPU + idx_to_use];

		/*
		 * no free record, so:
		 * we set our bit in cpu_bitmap and
		 * we spin to become a combiner
		 */
		while(bitmap32_test(&map->rec_bitmap[cpu], idx_to_use)) {
			bitmap64_set(&map->cpu_bitmap, cpu);
			fc_try_combiner(fc);
		}
	}
}

void fc_publish_record(struct flat_combining *fc, const int cpu)
{
	struct pub_list *map = &fc->map;
	int idx_to_use;

	idx_to_use = map->last_used_idx[cpu];
	map->last_used_idx[cpu] = (map->last_used_idx[cpu] + 1) % PUB_RECORD_PER_CPU;

	bitmap32_set(&map->rec_bitmap[cpu], idx_to_use);
	bitmap64_set(&map->cpu_bitmap, cpu);
}

void fc_try_combiner(struct flat_combining *fc)
{
	if(!fc_trylock(&fc->ds_lock)) {
		fc_do_combiner(fc);
		fc_unlock(&fc->ds_lock);
	}
}

void fc_data_structure_lock(struct flat_combining *fc)
{
	fc_lock(&fc->ds_lock);
}

void fc_data_structure_unlock(struct flat_combining *fc)
{
	fc_unlock(&fc->ds_lock);
}

void fc_print_publication_list(struct flat_combining *fc)
{
	struct pub_list *map = &fc->map;
	int i, cpu = smp_processor_id();

	trace_printk("[%d] - CPUs map: %llu\n", cpu, map->cpu_bitmap);
	for(i = 0; i < NR_CPUS; i++)
		trace_printk("[%d] - cpu %d map %u\n", cpu, i, map->rec_bitmap[i]);
}
