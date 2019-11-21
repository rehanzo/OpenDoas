/*
 * 1) Timestamp files and directories
 *
 * Timestamp files MUST NOT be accessible to users other than root,
 * this includes the name, metadata and the content of timestamp files
 * and directories.
 *
 * Symlinks can be used to create, manipulate or delete wrong files
 * and directories. The Implementation MUST reject any symlinks for
 * timestamp files or directories.
 *
 * To avoid race conditions the implementation MUST use the same
 * file descriptor for permission checks and do read or write
 * write operations after the permission checks.
 *
 * The timestamp files MUST be opened with openat(2) using the
 * timestamp directory file descriptor. Permissions of the directory
 * MUST be checked before opening the timestamp file descriptor.
 *
 * 2) Clock sources for timestamps
 *
 * Timestamp files MUST NOT rely on only one clock source, using the
 * wall clock would allow to reset the clock to an earlier point in
 * time to reuse a timestamp.
 *
 * The timestamp MUST consist of multiple clocks and MUST reject the
 * timestamp if there is a change to any clock because there is no way
 * to differentiate between malicious and legitimate clock changes.
 *
 * 3) Timestamp lifetime
 *
 * The implementation MUST NOT use the user controlled stdin, stdout
 * and stderr file descriptors to determine the controlling terminal.
 * On linux the /proc/$pid/stat file MUST be used to get the terminal
 * number.
 *
 * There is no reliable way to determine the lifetime of a tty/pty.
 * The start time of the session leader MUST be used as part of the
 * timestamp to determine if the tty is still the same.
 * If the start time of the session leader changed the timestamp MUST
 * be rejected.
 *
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#if !defined(timespecisset) || \
    !defined(timespeccmp) || \
    !defined(timespecadd)
#	include "sys-time.h"
#endif

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "includes.h"

#ifndef TIMESTAMP_DIR
#	define TIMESTAMP_DIR "/run/doas"
#endif

#if defined(TIMESTAMP_TMPFS) && defined(__linux__)
#	ifndef TMPFS_MAGIC
#		define TMPFS_MAGIC 0x01021994
#	endif
#endif

#ifdef __linux__
/* Use tty_nr from /proc/self/stat instead of using
 * ttyname(3), stdin, stdout and stderr are user
 * controllable and would allow to reuse timestamps
 * from another writable terminal.
 * See https://www.sudo.ws/alerts/tty_tickets.html
 */
static int
proc_info(pid_t pid, int *ttynr, unsigned long long *starttime)
{
	char path[128];
	char buf[1024];
	char *p, *saveptr, *ep;
	const char *errstr;
	int fd, n;

	p = buf;

	n = snprintf(path, sizeof path, "/proc/%d/stat", pid);
	if (n < 0 || n >= (int)sizeof path)
		return -1;

	if ((fd = open(path, O_RDONLY)) == -1)
		return -1;

	while ((n = read(fd, p, buf + sizeof buf - p)) != 0) {
		if (n == -1) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		p += n;
		if (p >= buf + sizeof buf)
			break;
	}
	close(fd);

	/* error if it contains NULL bytes */
	if (n != 0 || memchr(buf, '\0', p - buf))
		return -1;

	/* Get the 7th field, 5 fields after the last ')',
	 * (2th field) because the 5th field 'comm' can include
	 * spaces and closing paranthesis too.
	 * See https://www.sudo.ws/alerts/linux_tty.html
	 */
	if ((p = strrchr(buf, ')')) == NULL)
		return -1;

	n = 2;
	for ((p = strtok_r(p, " ", &saveptr)); p;
	    (p = strtok_r(NULL, " ", &saveptr))) {
		switch (n++) {
		case 7:
			*ttynr = strtonum(p, INT_MIN, INT_MAX, &errstr);
			if (errstr)
				return -1;
			break;
		case 22:
			errno = 0;
			*starttime = strtoull(p, &ep, 10);
			if (p == ep ||
			   (errno == ERANGE && *starttime == ULLONG_MAX))
				return -1;
			break;
		}
		if (n == 23)
			break;
	}

	return 0;
}
#else
#error "proc_info not implemented"
#endif

static int
timestamp_path(char *buf, size_t len)
{
	pid_t ppid, sid;
	unsigned long long starttime;
	int n, ttynr;

	ppid = getppid();
	if ((sid = getsid(0)) == -1)
		return -1;
	if (proc_info(ppid, &ttynr, &starttime) == -1)
		return -1;
	n = snprintf(buf, len, TIMESTAMP_DIR "/%d-%d-%d-%llu-%d",
	    ppid, sid, ttynr, starttime, getuid());
	if (n < 0 || n >= (int)len)
		return -1;
	return 0;
}

int
timestamp_set(int fd, int secs)
{
	struct timespec ts[2], timeout = { .tv_sec = secs, .tv_nsec = 0 };

	if (clock_gettime(CLOCK_BOOTTIME, &ts[0]) == -1 ||
	    clock_gettime(CLOCK_REALTIME, &ts[1]) == -1)
		return -1;

	timespecadd(&ts[0], &timeout, &ts[0]);
	timespecadd(&ts[1], &timeout, &ts[1]);
	return futimens(fd, ts);
}

/*
 * Returns 1 if the timestamp is valid, 0 if its invalid
 */
static int
timestamp_check(int fd, int secs)
{
	struct timespec ts[2], timeout = { .tv_sec = secs, .tv_nsec = 0 };
	struct stat st;

	if (fstat(fd, &st) == -1)
		return 0;

	if (!timespecisset(&st.st_atim) || !timespecisset(&st.st_mtim))
		return 0;

	if (clock_gettime(CLOCK_BOOTTIME, &ts[0]) == -1 ||
	    clock_gettime(CLOCK_REALTIME, &ts[1]) == -1)
		return 0;

	/* check if timestamp is too old */
	if (timespeccmp(&st.st_atim, &ts[0], <) ||
	    timespeccmp(&st.st_mtim, &ts[1], <))
		return 0;

	/* check if timestamp is too far in the future */
	timespecadd(&ts[0], &timeout, &ts[0]);
	timespecadd(&ts[1], &timeout, &ts[1]);
	if (timespeccmp(&st.st_atim, &ts[0], >) ||
	    timespeccmp(&st.st_mtim, &ts[1], >))
		return 0;

	return 1;
}

int
timestamp_open(int *valid, int secs)
{
	struct timespec ts[2] = {0};
	struct stat st;
	int fd;
	char path[256];
	int serrno = 0;

	*valid = 0;

	if (stat(TIMESTAMP_DIR, &st) == -1) {
		if (errno != ENOENT)
			return -1;
		if (mkdir(TIMESTAMP_DIR, 0700) == -1)
			return -1;
	} else if (st.st_uid != 0 || st.st_mode != (S_IFDIR | 0700)) {
		return -1;
	}

	if (timestamp_path(path, sizeof path) == -1)
		return -1;

	if (stat(path, &st) != -1 && (st.st_uid != 0 || st.st_gid != getgid()|| st.st_mode != (S_IFREG | 0000)))
		return -1;

	fd = open(path, O_RDONLY|O_NOFOLLOW);
	if (fd == -1) {
		char tmp[256];
		int n;

		n = snprintf(tmp, sizeof tmp, TIMESTAMP_DIR "/.tmp-%d", getpid());
		if (n < 0 || n >= (int)sizeof tmp)
			return -1;

		fd = open(tmp, O_RDONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0000);
		if (fd == -1)
			return -1;
		if (futimens(fd, ts) == -1 || rename(tmp, path) == -1) {
			serrno = errno;
			close(fd);
			unlink(tmp);
			errno = serrno;
			return -1;
		}
	} else {
		*valid = timestamp_check(fd, secs);
	}
	return fd;
}

int
timestamp_clear()
{
	char path[256];

	if (timestamp_path(path, sizeof path) == -1)
		return -1;
	if (unlink(path) == -1 && errno != ENOENT)
		return -1;
	return 0;
}
