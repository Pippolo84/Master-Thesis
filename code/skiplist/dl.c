/*
 * Deadline Scheduling Class (SCHED_DEADLINE)
 *
 * Earliest Deadline First (EDF) + Constant Bandwidth Server (CBS).
 *
 * Tasks that periodically executes their instances for less than their
 * runtime won't miss any of their deadlines.
 * Tasks that are not periodic or sporadic or that tries to execute more
 * than their reserved bandwidth will be slowed down (and may potentially
 * miss some of their deadlines), and won't affect any other task.
 *
 * Copyright (C) 2012 Dario Faggioli <raistlin@linux.it>,
 *                    Juri Lelli <juri.lelli@gmail.com>,
 *                    Michael Trimarchi <michael@amarulasolutions.com>,
 *                    Fabio Checconi <fabio@gandalf.sssup.it>
 */

static int pull_dl_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;

	if (likely(!dl_overloaded(this_rq)))
		goto out;

	cpu = cpudl_find(&this_rq->rd->pull_cpudl, this_rq->rd->dlo_mask, NULL, NULL);
	if(cpu == -1 || this_cpu == cpu)
		goto out;
	
	src_rq = cpu_rq(cpu);

	/* Might drop this_rq->lock */
	double_lock_balance(this_rq, src_rq);

	/*
	 * If the pullable task is no more on the
	 * runqueue, we're done with it
	 */
	if(src_rq->dl.dl_nr_running <= 1)
		goto skip;

	p = pick_next_earliest_dl_task(src_rq, this_cpu);

	/*
	 * We found a task to be pulled if:
	 *  - p can run on this cpu (otherwise pick_next_earliest_dl_task has returned NULL)
	 *  - it preempts our current (if there's one)
	 */
	if (p && (!this_rq->dl.dl_nr_running || 
		dl_time_before(p->dl.deadline, this_rq->dl.earliest_dl.curr))) {
		
		WARN_ON(p == src_rq->curr);
		WARN_ON(!p->on_rq);

		/*
		 * Then we pull iff p has actually an earlier
		 * deadline than the current task of its runqueue.
		 */
		if (dl_time_before(p->dl.deadline, src_rq->curr->dl.deadline)) 
			goto skip;

		ret = 1;

		deactivate_task(src_rq, p, 0);
		set_task_cpu(p, this_cpu);
		activate_task(this_rq, p, 0);

	}

skip:
	double_unlock_balance(this_rq, src_rq);
out:
	return ret;
}
