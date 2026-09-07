// SPDX-License-Identifier: GPL-2.0
/*
 * CYDA OS kernel interface: the agent as a kernel object.
 *
 * An agent is a thread that registered itself through /dev/cyda. The kernel
 * keeps a struct cyda_agent for it (id, priority, state, capability bitmap)
 * reachable from task_struct, releases it when the thread exits, and exposes
 * all agents under /sys/kernel/cyda/.
 *
 *   /dev/cyda                   ioctl: REGISTER, UPDATE, UNREGISTER, QUERY
 *   /sys/kernel/cyda/version    interface version
 *   /sys/kernel/cyda/count      registered agents
 *   /sys/kernel/cyda/agents     tid id priority state capabilities policy nice cpu_ns
 *
 * This is the foundation for an agent-aware scheduling class and for
 * capability checks at the syscall boundary (see docs/ROADMAP.md).
 */

#include <linux/cyda.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <uapi/linux/cyda.h>

#define CYDA_IFACE_VERSION "0.2"

static LIST_HEAD(cyda_agents);
static DEFINE_SPINLOCK(cyda_lock);
static unsigned int cyda_count;
static struct kobject *cyda_kobj;

static const char *cyda_state_name(u32 state)
{
	switch (state) {
	case CYDA_STATE_NORMAL:
		return "normal";
	case CYDA_STATE_CRITICAL:
		return "critical";
	case CYDA_STATE_THROTTLED:
		return "throttled";
	case CYDA_STATE_SUSPENDED:
		return "suspended";
	default:
		return "unknown";
	}
}

/* Called from do_exit(): drop the agent bound to the exiting thread. */
void cyda_task_exit(struct task_struct *tsk)
{
	struct cyda_agent *a = tsk->cyda_agent;

	if (!a)
		return;
	tsk->cyda_agent = NULL;
	spin_lock(&cyda_lock);
	list_del(&a->node);
	cyda_count--;
	spin_unlock(&cyda_lock);
	pr_info("cyda: agent %s left (tid %d)\n", a->id, a->tid);
	kfree(a);
}

static long cyda_register(struct cyda_agent_reg __user *uarg)
{
	struct cyda_agent_reg reg;
	struct cyda_agent *a;

	if (copy_from_user(&reg, uarg, sizeof(reg)))
		return -EFAULT;
	if (current->cyda_agent)
		return -EEXIST;
	if (reg.priority > 100)
		return -EINVAL;
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	strscpy(a->id, reg.id, sizeof(a->id));
	if (!a->id[0]) {
		kfree(a);
		return -EINVAL;
	}
	a->task = current;
	a->tid = task_pid_nr(current);
	a->priority = reg.priority;
	a->state = reg.state;
	a->capabilities = reg.capabilities;
	a->registered_ns = ktime_get_ns();
	INIT_LIST_HEAD(&a->node);
	spin_lock(&cyda_lock);
	list_add_tail(&a->node, &cyda_agents);
	cyda_count++;
	spin_unlock(&cyda_lock);
	current->cyda_agent = a;
	pr_info("cyda: agent %s registered on tid %d (priority %u, capabilities %#llx)\n",
		a->id, a->tid, a->priority, (unsigned long long)a->capabilities);
	return 0;
}

static long cyda_update(struct cyda_agent_reg __user *uarg)
{
	struct cyda_agent_reg reg;
	struct cyda_agent *a = current->cyda_agent;

	if (!a)
		return -ENOENT;
	if (copy_from_user(&reg, uarg, sizeof(reg)))
		return -EFAULT;
	if (reg.priority > 100)
		return -EINVAL;
	spin_lock(&cyda_lock);
	a->priority = reg.priority;
	a->state = reg.state;
	a->capabilities = reg.capabilities;
	spin_unlock(&cyda_lock);
	return 0;
}

static long cyda_query(struct cyda_agent_query __user *uarg)
{
	struct cyda_agent_query q;
	struct cyda_agent *a;
	int found = 0;

	if (copy_from_user(&q, uarg, sizeof(q)))
		return -EFAULT;
	spin_lock(&cyda_lock);
	list_for_each_entry(a, &cyda_agents, node) {
		if (a->tid != q.tid)
			continue;
		memset(&q.agent, 0, sizeof(q.agent));
		strscpy(q.agent.id, a->id, sizeof(q.agent.id));
		q.agent.priority = a->priority;
		q.agent.state = a->state;
		q.agent.capabilities = a->capabilities;
		found = 1;
		break;
	}
	spin_unlock(&cyda_lock);
	if (!found)
		return -ENOENT;
	if (copy_to_user(uarg, &q, sizeof(q)))
		return -EFAULT;
	return 0;
}

static long cyda_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case CYDA_IOC_REGISTER:
		return cyda_register((struct cyda_agent_reg __user *)arg);
	case CYDA_IOC_UPDATE:
		return cyda_update((struct cyda_agent_reg __user *)arg);
	case CYDA_IOC_UNREGISTER:
		if (!current->cyda_agent)
			return -ENOENT;
		cyda_task_exit(current);
		return 0;
	case CYDA_IOC_QUERY:
		return cyda_query((struct cyda_agent_query __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations cyda_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = cyda_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice cyda_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cyda",
	.fops = &cyda_fops,
	.mode = 0600,
};

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CYDA_IFACE_VERSION);
}

static ssize_t count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int n;

	spin_lock(&cyda_lock);
	n = cyda_count;
	spin_unlock(&cyda_lock);
	return sysfs_emit(buf, "%u\n", n);
}

static ssize_t agents_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct cyda_agent *a;
	ssize_t len = 0;

	spin_lock(&cyda_lock);
	list_for_each_entry(a, &cyda_agents, node) {
		u64 utime, stime;

		task_cputime(a->task, &utime, &stime);
		len += sysfs_emit_at(buf, len, "%d %s %u %s %#llx %d %d %llu\n", a->tid, a->id,
				     a->priority, cyda_state_name(a->state),
				     (unsigned long long)a->capabilities, a->task->policy,
				     task_nice(a->task), (unsigned long long)(utime + stime));
		if (len >= PAGE_SIZE - 96)
			break;
	}
	spin_unlock(&cyda_lock);
	return len;
}

static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute count_attr = __ATTR_RO(count);
static struct kobj_attribute agents_attr = __ATTR_RO(agents);

static struct attribute *cyda_attrs[] = {
	&version_attr.attr,
	&count_attr.attr,
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
	if (ret)
		goto out_kobj;
	ret = misc_register(&cyda_dev);
	if (ret)
		goto out_group;
	pr_info("CYDA OS kernel interface v%s: the agent is the process\n", CYDA_IFACE_VERSION);
	return 0;
out_group:
	sysfs_remove_group(cyda_kobj, &cyda_group);
out_kobj:
	kobject_put(cyda_kobj);
	return ret;
}
subsys_initcall(cyda_init);
