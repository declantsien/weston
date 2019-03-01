/*
 * Copyright © 2018 Collabora, Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <sys/ioctl.h>

#ifdef HAVE_LINUX_SYNC_FILE_H
#include <linux/sync_file.h>
#else
#include "linux-sync-file-uapi.h"
#endif

#include "linux-sync-file.h"
#include "shared/timespec-util.h"

/* Check that a file descriptor represents a valid sync file
 *
 * \param fd[in] a file descriptor
 * \return true if fd is a valid sync file, false otherwise
 */
bool
linux_sync_file_is_valid(int fd)
{
	struct sync_file_info file_info = { { 0 } };

	if (ioctl(fd, SYNC_IOC_FILE_INFO, &file_info) < 0)
		return false;

	return file_info.num_fences > 0;
}

/* Read the timestamp stored in a sync file
 *
 * \param fd[in] fd a file descriptor for a sync file
 * \param ts[out] the timespec struct to fill with the timestamp
 * \return 0 if a timestamp was read, -1 on error
 */
int
linux_sync_file_read_timestamp(int fd, struct timespec *ts)
{
	struct sync_file_info file_info = { { 0 } };
	struct sync_fence_info fence_info = { { 0 } };

	assert(ts != NULL);

	file_info.sync_fence_info = (uint64_t)(uintptr_t)&fence_info;
	file_info.num_fences = 1;

	if (ioctl(fd, SYNC_IOC_FILE_INFO, &file_info) < 0)
		return -1;

	timespec_from_nsec(ts, fence_info.timestamp_ns);

	return 0;
}

/* Merge two sync file fds
 *
 * Creates a new sync file fd that will become ready when
 * both the source sync file fds are ready.
 *
 * \param fd1[in] the first sync file fd to merge
 * \param fd2[in] the second sync file fd to merge
 * \return the merged sync file fd, -1 on error
 */
int
linux_sync_file_merge(int fd1, int fd2)
{
	struct sync_merge_data merge_data = { { 0 } };

	merge_data.fd2 = fd2;

	if (ioctl(fd1, SYNC_IOC_MERGE, &merge_data) < 0)
		return -1;

	return merge_data.fence;
}
