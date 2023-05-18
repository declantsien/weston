/* Sync file Linux kernel UAPI */

#ifndef WESTON_LINUX_SYNC_FILE_UAPI_H
#define WESTON_LINUX_SYNC_FILE_UAPI_H

#ifdef __linux__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
typedef uint64_t __u64;
typedef int32_t __s32;
typedef uint32_t __u32;
#endif

struct sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	__s32 status;
	__u32 flags;
	__u64 timestamp_ns;
};

struct sync_file_info {
	char name[32];
	__s32 status;
	__u32 flags;
	__u32 num_fences;
	__u32 pad;

	__u64 sync_fence_info;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)

#endif /* WESTON_LINUX_SYNC_FILE_UAPI_H */
