#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/kernel_stat.h>

#include "sched.h"

/*
 * CPU accounting code for task groups.
 *
 * Based on the work by Paul Menage (menage@google.com) and Balbir Singh
 * (balbir@in.ibm.com).
 */

struct cpuacct root_cpuacct;

/* create a new cpu accounting group */
static struct cgroup_subsys_state *cpuacct_css_alloc(struct cgroup *cgrp)
{
	struct cpuacct *ca;

	if (!cgrp->parent)
		return &root_cpuacct.css;

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca)
		goto out;

	ca->cpuusage = alloc_percpu(u64);
	if (!ca->cpuusage)
		goto out_free_ca;

	ca->cpustat = alloc_percpu(struct kernel_cpustat);
	if (!ca->cpustat)
		goto out_free_cpuusage;

	return &ca->css;

out_free_cpuusage:
	free_percpu(ca->cpuusage);
out_free_ca:
	kfree(ca);
out:
	return ERR_PTR(-ENOMEM);
}

/* destroy an existing cpu accounting group */
static void cpuacct_css_free(struct cgroup *cgrp)
{
	struct cpuacct *ca = cgroup_ca(cgrp);

	free_percpu(ca->cpustat);
	free_percpu(ca->cpuusage);
	kfree(ca);
}

static u64 cpuacct_cpuusage_read(struct cpuacct *ca, int cpu)
{
	u64 *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	u64 data;

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit read safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
	data = *cpuusage;
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#else
	data = *cpuusage;
#endif

	return data;
}

static void cpuacct_cpuusage_write(struct cpuacct *ca, int cpu, u64 val)
{
	u64 *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit write safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
	*cpuusage = val;
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#else
	*cpuusage = val;
#endif
}

/* return total cpu usage (in nanoseconds) of a group */
static u64 cpuusage_read(struct cgroup *cgrp, struct cftype *cft)
{
	struct cpuacct *ca = cgroup_ca(cgrp);
	u64 totalcpuusage = 0;
	int i;

	for_each_present_cpu(i)
		totalcpuusage += cpuacct_cpuusage_read(ca, i);

	return totalcpuusage;
}

static int cpuusage_write(struct cgroup *cgrp, struct cftype *cftype,
								u64 reset)
{
	struct cpuacct *ca = cgroup_ca(cgrp);
	int err = 0;
	int i;

	if (reset) {
		err = -EINVAL;
		goto out;
	}

	for_each_present_cpu(i)
		cpuacct_cpuusage_write(ca, i, 0);

out:
	return err;
}

static int cpuacct_percpu_seq_read(struct cgroup *cgroup, struct cftype *cft,
				   struct seq_file *m)
{
	struct cpuacct *ca = cgroup_ca(cgroup);
	u64 percpu;
	int i;

	for_each_present_cpu(i) {
		percpu = cpuacct_cpuusage_read(ca, i);
		seq_printf(m, "%llu ", (unsigned long long) percpu);
	}
	seq_printf(m, "\n");
	return 0;
}

static const char * const cpuacct_stat_desc[] = {
	[CPUACCT_STAT_USER] = "user",
	[CPUACCT_STAT_SYSTEM] = "system",
};

static int cpuacct_stats_show(struct cgroup *cgrp, struct cftype *cft,
			      struct cgroup_map_cb *cb)
{
	struct cpuacct *ca = cgroup_ca(cgrp);
	int cpu;
	s64 val = 0;

	for_each_online_cpu(cpu) {
		struct kernel_cpustat *kcpustat = per_cpu_ptr(ca->cpustat, cpu);
		val += kcpustat->cpustat[CPUTIME_USER];
		val += kcpustat->cpustat[CPUTIME_NICE];
	}
	val = cputime64_to_clock_t(val);
	cb->fill(cb, cpuacct_stat_desc[CPUACCT_STAT_USER], val);

	val = 0;
	for_each_online_cpu(cpu) {
		struct kernel_cpustat *kcpustat = per_cpu_ptr(ca->cpustat, cpu);
		val += kcpustat->cpustat[CPUTIME_SYSTEM];
		val += kcpustat->cpustat[CPUTIME_IRQ];
		val += kcpustat->cpustat[CPUTIME_SOFTIRQ];
	}

	val = cputime64_to_clock_t(val);
	cb->fill(cb, cpuacct_stat_desc[CPUACCT_STAT_SYSTEM], val);

	return 0;
}

static struct cftype files[] = {
	{
		.name = "usage",
		.read_u64 = cpuusage_read,
		.write_u64 = cpuusage_write,
	},
	{
		.name = "usage_percpu",
		.read_seq_string = cpuacct_percpu_seq_read,
	},
	{
		.name = "stat",
		.read_map = cpuacct_stats_show,
	},
	{ }	/* terminate */
};

/*
 * charge this task's execution time to its accounting group.
 *
 * called with rq->lock held.
 */
void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
	struct cpuacct *ca;
	int cpu;

	if (unlikely(!cpuacct_subsys.active))
		return;

	cpu = task_cpu(tsk);

	rcu_read_lock();

	ca = task_ca(tsk);

	for (; ca; ca = parent_ca(ca)) {
		u64 *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
		*cpuusage += cputime;
	}

	rcu_read_unlock();
}

struct cgroup_subsys cpuacct_subsys = {
	.name = "cpuacct",
	.css_alloc = cpuacct_css_alloc,
	.css_free = cpuacct_css_free,
	.subsys_id = cpuacct_subsys_id,
	.base_cftypes = files,
};
