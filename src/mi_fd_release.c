#include "mi_fd_release.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* Release order of every healthy waybeam exit (kmsg disconnect lines,
 * cal5 16/16 transitions): vif, vpe, venc, sensor, then sys. */
static const char *const k_first[] = { "vif", "vpe", "venc", "sensor" };

typedef struct {
	int fd;
	char name[MI_FD_NAME_MAX];
} MiFd;

const char *mi_fd_module(const char *target, const char *dev_root)
{
	size_t n;
	const char *rest;

	if (!target || !dev_root)
		return NULL;
	n = strlen(dev_root);
	if (strncmp(target, dev_root, n) != 0)
		return NULL;
	rest = target + n;
	if (strncmp(rest, "/mi_", 4) == 0 || strncmp(rest, "/mi/", 4) == 0)
		rest += 4;
	else
		return NULL;
	if (rest[0] == '\0' || strchr(rest, '/') ||
	    strlen(rest) >= MI_FD_NAME_MAX)
		return NULL;
	return rest;
}

/* Every open MI fd except sys, in fd order. */
static int scan_non_sys(const char *fd_dir, const char *dev_root,
	MiFd *out, int max)
{
	DIR *d = opendir(fd_dir);
	struct dirent *ent;
	int n = 0;

	if (!d)
		return 0;
	while ((ent = readdir(d)) != NULL && n < max) {
		char link[512];
		char target[256];
		const char *mod;
		char *end;
		long fd;
		ssize_t len;

		fd = strtol(ent->d_name, &end, 10);
		if (end == ent->d_name || *end != '\0' || fd < 0 || fd == dirfd(d))
			continue;
		snprintf(link, sizeof(link), "%s/%s", fd_dir, ent->d_name);
		len = readlink(link, target, sizeof(target) - 1);
		if (len <= 0)
			continue;
		target[len] = '\0';
		mod = mi_fd_module(target, dev_root);
		if (!mod || strcmp(mod, "sys") == 0)
			continue;
		out[n].fd = (int)fd;
		snprintf(out[n].name, sizeof(out[n].name), "%s", mod);
		n++;
	}
	closedir(d);

	/* readdir order is not fd order; sort ascending (small n). */
	for (int i = 1; i < n; i++) {
		MiFd k = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].fd > k.fd) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = k;
	}
	return n;
}

static int rank_of(const char *name)
{
	for (size_t i = 0; i < sizeof(k_first) / sizeof(k_first[0]); i++)
		if (strcmp(name, k_first[i]) == 0)
			return (int)i;
	return (int)(sizeof(k_first) / sizeof(k_first[0]));
}

int mi_fd_release_non_sys_in(const char *fd_dir, const char *dev_root,
	char names[][MI_FD_NAME_MAX], int max_names)
{
	MiFd fds[MI_FD_RELEASE_MAX];
	int n = scan_non_sys(fd_dir, dev_root, fds, MI_FD_RELEASE_MAX);
	int closed = 0;
	int last_rank = (int)(sizeof(k_first) / sizeof(k_first[0]));

	/* Known modules in healthy-exit order, then the rest in fd order. */
	for (int r = 0; r <= last_rank; r++) {
		for (int i = 0; i < n; i++) {
			if (fds[i].fd < 0 || rank_of(fds[i].name) != r)
				continue;
			close(fds[i].fd);
			fds[i].fd = -1;
			if (names && closed < max_names)
				snprintf(names[closed], MI_FD_NAME_MAX, "%s",
					fds[i].name);
			closed++;
		}
	}
	return closed;
}

int mi_fd_release_non_sys(void)
{
	char names[MI_FD_RELEASE_MAX][MI_FD_NAME_MAX];
	char line[MI_FD_RELEASE_MAX * (MI_FD_NAME_MAX + 1) + 1];
	size_t off = 0;
	int n;

	memset(names, 0, sizeof(names));
	n = mi_fd_release_non_sys_in("/proc/self/fd", "/dev", names,
		MI_FD_RELEASE_MAX);
	line[0] = '\0';
	for (int i = 0; i < n && i < MI_FD_RELEASE_MAX; i++)
		off += (size_t)snprintf(line + off, sizeof(line) - off, " %s",
			names[i]);
	fprintf(stderr, "> MI fds released before exit:%s (%d); sys left for exit\n",
		n ? line : " none", n);
	return n;
}

void mi_fd_close_from(int lowfd)
{
	struct rlimit rl;
	long max = 1024;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		max = (long)rl.rlim_cur;
	if (max > 65536)
		max = 65536;
	for (long fd = lowfd; fd < max; fd++)
		close((int)fd);
}
