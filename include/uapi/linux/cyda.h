/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * CYDA OS user API: /dev/cyda
 *
 * A thread registers itself as an agent; the kernel then knows its id,
 * priority, state and capabilities for as long as the thread lives.
 */
#ifndef _UAPI_LINUX_CYDA_H
#define _UAPI_LINUX_CYDA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CYDA_ID_LEN 32

#define CYDA_STATE_NORMAL	0
#define CYDA_STATE_CRITICAL	1
#define CYDA_STATE_THROTTLED	2
#define CYDA_STATE_SUSPENDED	3

/* Capability bits (mirrored by cyda-core in user space). */
#define CYDA_CAP_PLC_READ		(1ULL << 0)
#define CYDA_CAP_PLC_WRITE		(1ULL << 1)
#define CYDA_CAP_OPCUA_READ		(1ULL << 2)
#define CYDA_CAP_MODBUS_READ		(1ULL << 3)
#define CYDA_CAP_MODBUS_WRITE		(1ULL << 4)
#define CYDA_CAP_CAMERA_READ		(1ULL << 5)
#define CYDA_CAP_SENSOR_READ		(1ULL << 6)
#define CYDA_CAP_GPU_USE		(1ULL << 7)
#define CYDA_CAP_ALARM_CREATE		(1ULL << 8)
#define CYDA_CAP_MACHINE_STOP		(1ULL << 9)
#define CYDA_CAP_MACHINE_START		(1ULL << 10)
#define CYDA_CAP_RECIPE_CHANGE		(1ULL << 11)
#define CYDA_CAP_MAINTENANCE_REQUEST	(1ULL << 12)
#define CYDA_CAP_RESOURCE_READ		(1ULL << 13)
#define CYDA_CAP_CUSTOM			(1ULL << 63)

struct cyda_agent_reg {
	char id[CYDA_ID_LEN];
	__u32 priority;
	__u32 state;
	__u64 capabilities;
};

struct cyda_agent_query {
	__s32 tid;
	__u32 pad;
	struct cyda_agent_reg agent;
};

#define CYDA_IOC_MAGIC 'C'
#define CYDA_IOC_REGISTER	_IOW(CYDA_IOC_MAGIC, 1, struct cyda_agent_reg)
#define CYDA_IOC_UNREGISTER	_IO(CYDA_IOC_MAGIC, 2)
#define CYDA_IOC_UPDATE		_IOW(CYDA_IOC_MAGIC, 3, struct cyda_agent_reg)
#define CYDA_IOC_QUERY		_IOWR(CYDA_IOC_MAGIC, 4, struct cyda_agent_query)

#endif /* _UAPI_LINUX_CYDA_H */
