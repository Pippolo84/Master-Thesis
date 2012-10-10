/*
 *  kernel/sched/bm_fc.h
 *
 *  Bitmap Flat Combining header file
 *
 *  Author: Fabio Falzoi <fabio.falzoi@alice.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#ifndef __BM_FC_H
#define __BM_FC_H

#include <linux/bitops.h>

/* flat combining parameters */

/*
 * no more than 32 publication 
 * records allowed in this 
 * implementation
 */
#define PUB_RECORD_PER_CPU		10

/* data structure operations type */
typedef enum {
	SET
} op_type;

/* data structure operations parameters */
typedef struct {
	int cpu;
	u64 dline;
	int is_valid;
} set_params;

typedef union {
	set_params set_p;
} params;

/* data structure operations handler */
typedef struct {
	void (*function)(void *data_structure, int cpu, u64 dline, int is_valid);
} set_handler;

typedef union {
	set_handler set_h;
} handler;

/* publication record */
struct pub_record {
	/* operation type */
	op_type req;
	/* operation parameters */
	params par;
	/* operation handler */
	handler h;
};

/* data structure lock interface */
#define DS_LOCKED	1
#define DS_UNLOCKED	0

struct data_structure_lock {
	atomic_t lock;
};

/* publication record list */
struct pub_list {
	/* publisher CPUs bitmap */
	u64 cpu_bitmap;
	/* active publication records bitmap */
	u32 rec_bitmap[NR_CPUS];
	/* publication record array */
	struct pub_record rec_array[NR_CPUS * PUB_RECORD_PER_CPU];
	/* last used per CPU publication record index */
	int last_used_idx[NR_CPUS];
};

/* flat combining helper structure */
struct flat_combining {
	/* concurrent data structure */
	void *data_structure;
	/* publication list */
	struct pub_list map;
	/* data structure lock */
	struct data_structure_lock ds_lock;
	/* cache cpu */
	atomic_t cached_cpu;
	/* dl array */
	atomic64_t current_dl[NR_CPUS];
};

/* flat combining interface */
struct flat_combining *fc_create(void *data_structure);

int fc_destroy(struct flat_combining *fc);

struct pub_record *fc_get_record(struct flat_combining *fc, const int cpu);

void fc_publish_record(struct flat_combining *fc, const int cpu);

/*
 * if we use a totally asynchronous flat combining
 * implementation, this function is going to be
 * used only internally in this module.
 * Otherwise, when we want to stop deferring work,
 * we have to call it explicitly.
 */
void fc_try_combiner(struct flat_combining *fc);

/*
 * if we want to ensure that a certain operation
 * will be executed synchronously and sequentially
 * we have to acquire and further release lock
 * on data structure with these functions
 */
void fc_data_structure_lock(struct flat_combining *fc);

void fc_data_structure_unlock(struct flat_combining *fc);

/* helper function useful for debugging purpose */
void fc_print_publication_list(struct flat_combining *fc);

#endif /* __BM_FC_H */
