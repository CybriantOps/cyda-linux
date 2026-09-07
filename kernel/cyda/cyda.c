// SPDX-License-Identifier: GPL-2.0
/*
 * CYDA OS kernel interface.
 *
 * First step of an agent-aware kernel: the kernel exposes, under
 * /sys/kernel/cyda/, what it knows about CYDA agents. Today an agent is
 * identified by its thread name ("agent:<id>", set by the CYDA runtime);
 * later versions carry a proper agent object (goal, capabilities, resource
 * leases) and an agent-aware scheduling class.
 *
 *   /sys/kernel/cyda/version   interface version
 *   /sys/kernel/cyda/agents    one line per agent thread: tid  comm  policy  nice  cpu_ns
 */

#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define CYDA_IFACE_VERSION "0.1"
#define CYDA_AGENT_PREFIX "agent:"

static struct kobject *cyda_kobj;

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CYDA_IFACE_VERSION);
}

static ssize_t agents_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct task_struct *p, *t;
	ssize_t len = 0;

	rcu_read_lock();
	for_each_process_thread(p, t) {
		u64 utime, stime;

		if (strncmp(t->comm, CYDA_AGENT_PREFIX, strlen(CYDA_AGENT_PREFIX)) != 0)
			continue;
		task_cputime(t, &utime, &stime);
		len += sysfs_emit_at(buf, len, "%d %s %d %d %llu\n", task_pid_nr(t),
				     t->comm + strlen(CYDA_AGENT_PREFIX), t->policy,
				     task_nice(t), (unsigned long long)(utime + stime));
		if (len >= PAGE_SIZE - 64)
			break;
	}
	rcu_read_unlock();
	return len;
}

static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute agents_attr = __ATTR_RO(agents);

static struct attribute *cyda_attrs[] = {
	&version_attr.attr,
	&agents_attr.attr,
	NULL,
};

static const struct attribute_group cyda_group = {
	.attrs = cyda_attrs,
};

static int __init cyda_init(void)
{
	int ret;

	cyda_kobj = kobject_create_and_add("cyda", kernel_kobj);
	if (!cyda_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(cyda_kobj, &cyda_group);
	if (ret) {
		kobject_put(cyda_kobj);
		return ret;
	}
	pr_info("CYDA OS kernel interface v%s: the agent is the process\n", CYDA_IFACE_VERSION);
	return 0;
}
subsys_initcall(cyda_init);
