// SPDX-License-Identifier: GPL-2.0
/*
 * CYDA capability LSM.
 *
 * The CYDA system layer decides what an agent may do (capability check,
 * policy check, human authorization). This module makes the kernel the
 * last line: a thread that registered as an agent (task_struct::cyda_agent)
 * cannot reach a device or a network endpoint its capability bitmap does
 * not cover, and cannot change its own scheduling.
 *
 *   socket_connect     AF_INET/AF_INET6 needs a PLC/Modbus/OPC-UA/RESOURCE_READ capability,
 *                      and the destination must be one of the agent's endpoints
 *                      (CYDA_IOC_SET_ENDPOINTS) when it has a list
 *   file_open          /dev/video* (major 81) needs CAMERA_READ;
 *                      DRM (226) / NVIDIA (195) need GPU_USE;
 *                      writing cgroup files is never allowed for an agent
 *   task_setscheduler  an agent never reschedules itself or others
 *   task_setnice
 *
 * Denials return -EPERM, are counted in /sys/kernel/cyda/denied and are
 * logged (rate limited). Threads that are not agents are unaffected.
 */

#include <linux/cyda.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/lsm_hooks.h>
#include <linux/magic.h>
#include <linux/net.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/string.h>
#include <uapi/linux/cyda.h>
#include <uapi/linux/lsm.h>

#define VIDEO_MAJOR 81
#define DRM_MAJOR 226
#define NVIDIA_MAJOR 195

#define CYDA_CAP_NET (CYDA_CAP_PLC_READ | CYDA_CAP_PLC_WRITE | CYDA_CAP_OPCUA_READ | \
		      CYDA_CAP_MODBUS_READ | CYDA_CAP_MODBUS_WRITE | CYDA_CAP_RESOURCE_READ)

static int cyda_deny(struct cyda_agent *a, const char *what)
{
	cyda_agent_denied();
	pr_warn_ratelimited("cyda: agent %s (tid %d) denied: %s (capabilities %#llx)\n",
			    a->id, a->tid, what, (unsigned long long)a->capabilities);
	return -EPERM;
}

/* Does the destination match one of the agent's allowed endpoints? */
static bool cyda_endpoint_allowed(struct cyda_agent *a, struct sockaddr *address, int addrlen)
{
	unsigned int i;
	u16 port;
	const u8 *addr;
	size_t alen;

	if (address->sa_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)address;

		if (addrlen < (int)sizeof(*in))
			return false;
		port = ntohs(in->sin_port);
		addr = (const u8 *)&in->sin_addr;
		alen = 4;
	} else {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)address;

		if (addrlen < (int)sizeof(*in6))
			return false;
		port = ntohs(in6->sin6_port);
		addr = (const u8 *)&in6->sin6_addr;
		alen = 16;
	}
	for (i = 0; i < a->endpoint_count && i < CYDA_MAX_ENDPOINTS; i++) {
		const struct cyda_endpoint *e = &a->endpoints[i];

		if (e->family != address->sa_family)
			continue;
		if (e->port && e->port != port)
			continue;
		if (memcmp(e->addr, addr, alen) == 0)
			return true;
	}
	return false;
}

static int cyda_socket_connect(struct socket *sock, struct sockaddr *address, int addrlen)
{
	struct cyda_agent *a = current->cyda_agent;
	bool allowed;

	if (!a)
		return 0;
	if (address->sa_family != AF_INET && address->sa_family != AF_INET6)
		return 0;
	if (!(a->capabilities & CYDA_CAP_NET))
		return cyda_deny(a, "network connect without a device capability");
	if (!a->endpoint_count)
		return 0;
	spin_lock(&cyda_agent_lock);
	allowed = cyda_endpoint_allowed(a, address, addrlen);
	spin_unlock(&cyda_agent_lock);
	if (allowed)
		return 0;
	return cyda_deny(a, "network connect outside the agent's endpoints");
}

static int cyda_file_open(struct file *file)
{
	struct cyda_agent *a = current->cyda_agent;
	struct inode *inode;

	if (!a)
		return 0;
	inode = file_inode(file);
	if (S_ISCHR(inode->i_mode)) {
		unsigned int major = MAJOR(inode->i_rdev);

		if (major == VIDEO_MAJOR && !(a->capabilities & CYDA_CAP_CAMERA_READ))
			return cyda_deny(a, "camera device without CAMERA_READ");
		if ((major == DRM_MAJOR || major == NVIDIA_MAJOR) && !(a->capabilities & CYDA_CAP_GPU_USE))
			return cyda_deny(a, "GPU device without GPU_USE");
		return 0;
	}
	if ((file->f_mode & FMODE_WRITE) && inode->i_sb->s_magic == CGROUP2_SUPER_MAGIC)
		return cyda_deny(a, "cgroup write from an agent");
	return 0;
}

static int cyda_task_setscheduler(struct task_struct *p)
{
	struct cyda_agent *a = current->cyda_agent;

	if (!a)
		return 0;
	return cyda_deny(a, "scheduler change from an agent");
}

static int cyda_task_setnice(struct task_struct *p, int nice)
{
	struct cyda_agent *a = current->cyda_agent;

	if (!a)
		return 0;
	return cyda_deny(a, "nice change from an agent");
}

static const struct lsm_id cyda_lsmid = {
	.name = "cyda",
	.id = LSM_ID_CYDA,
};

static struct security_hook_list cyda_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(socket_connect, cyda_socket_connect),
	LSM_HOOK_INIT(file_open, cyda_file_open),
	LSM_HOOK_INIT(task_setscheduler, cyda_task_setscheduler),
	LSM_HOOK_INIT(task_setnice, cyda_task_setnice),
};

static int __init cyda_lsm_init(void)
{
	pr_info("CYDA: capability LSM active — agents cannot exceed their capabilities\n");
	security_add_hooks(cyda_hooks, ARRAY_SIZE(cyda_hooks), &cyda_lsmid);
	return 0;
}

DEFINE_LSM(cyda) = {
	.name = "cyda",
	.init = cyda_lsm_init,
};
