/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CYDA_H
#define _LINUX_CYDA_H

#include <linux/list.h>
#include <linux/types.h>

struct task_struct;

/* Kernel-side identity of a CYDA agent, bound to one thread. */
struct cyda_agent {
	struct list_head node;
	struct task_struct *task;
	pid_t tid;
	char id[32];
	u32 priority;		/* 0..100, the CYDA scheduler's effective priority */
	u32 state;		/* CYDA_STATE_* */
	u64 capabilities;	/* CYDA_CAP_* bitmap */
	u64 registered_ns;
};

#ifdef CONFIG_CYDA
void cyda_task_exit(struct task_struct *tsk);
#else
static inline void cyda_task_exit(struct task_struct *tsk) { }
#endif

#endif /* _LINUX_CYDA_H */
