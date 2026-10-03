#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_fd_release.h"
#include "test_helpers.h"

/* F2 (no_frames): a clean exit must leave "sys" as the only MI device fd for
 * the exit path, so the last MI release under PF_EXITING is always "sys".
 * The device files are faked under a temp dev root; the fd scan is the real
 * /proc/self/fd of the test process. */

static int fd_open(int fd)
{
	return fcntl(fd, F_GETFD) != -1;
}

static int fd_closed(int fd)
{
	return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

static int touch_open(const char *root, const char *rel)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s", root, rel);
	int fd = open(p, O_RDWR | O_CREAT, 0600);
	return fd;
}

static int test_module_name(void)
{
	int failures = 0;
	const char *m;

	m = mi_fd_module("/dev/mi_sys", "/dev");
	CHECK("mi_fd_module_sys", m && strcmp(m, "sys") == 0);
	m = mi_fd_module("/dev/mi_sensor", "/dev");
	CHECK("mi_fd_module_sensor", m && strcmp(m, "sensor") == 0);
	m = mi_fd_module("/dev/mi/vpe", "/dev");
	CHECK("mi_fd_module_subdir_form", m && strcmp(m, "vpe") == 0);
	m = mi_fd_module("/tmp/x/dev/mi_venc", "/tmp/x/dev");
	CHECK("mi_fd_module_custom_root", m && strcmp(m, "venc") == 0);
	CHECK("mi_fd_module_not_mi", mi_fd_module("/dev/misc", "/dev") == NULL);
	CHECK("mi_fd_module_empty_name", mi_fd_module("/dev/mi_", "/dev") == NULL);
	CHECK("mi_fd_module_nested", mi_fd_module("/dev/mi/a/b", "/dev") == NULL);
	CHECK("mi_fd_module_other_root", mi_fd_module("/dev/mi_sys", "/tmp/dev") == NULL);
	CHECK("mi_fd_module_socket", mi_fd_module("socket:[1234]", "/dev") == NULL);
	CHECK("mi_fd_module_null", mi_fd_module(NULL, "/dev") == NULL);
	return failures;
}

static int test_release_order(void)
{
	int failures = 0;
	char root[] = "/tmp/wb_mi_fd_XXXXXX";
	char dev[300];
	char names[MI_FD_RELEASE_MAX][MI_FD_NAME_MAX];
	int fsys, fvenc, fvpe, fvif, fsnr, fpoll, fother, fmisc, n;

	if (!mkdtemp(root)) {
		CHECK("mi_fd_release_mkdtemp", 0);
		return failures;
	}
	snprintf(dev, sizeof(dev), "%s/dev", root);
	mkdir(dev, 0700);

	/* Same open order as a live waybeam's kmsg connect lines:
	 * sys, venc, vpe, vif, sensor (then a poll fd, then two non-MI files). */
	fsys = touch_open(dev, "mi_sys");
	fvenc = touch_open(dev, "mi_venc");
	fvpe = touch_open(dev, "mi_vpe");
	fvif = touch_open(dev, "mi_vif");
	fsnr = touch_open(dev, "mi_sensor");
	fpoll = touch_open(dev, "mi_poll");
	fother = touch_open(dev, "other");
	fmisc = touch_open(dev, "misc");
	CHECK("mi_fd_release_setup", fsys >= 0 && fvenc >= 0 && fvpe >= 0 &&
		fvif >= 0 && fsnr >= 0 && fpoll >= 0 && fother >= 0 && fmisc >= 0);

	memset(names, 0, sizeof(names));
	n = mi_fd_release_non_sys_in("/proc/self/fd", dev, names, MI_FD_RELEASE_MAX);
	CHECK("mi_fd_release_count_5", n == 5);
	CHECK("mi_fd_release_sys_kept", fd_open(fsys));
	CHECK("mi_fd_release_venc_closed", fd_closed(fvenc));
	CHECK("mi_fd_release_vpe_closed", fd_closed(fvpe));
	CHECK("mi_fd_release_vif_closed", fd_closed(fvif));
	CHECK("mi_fd_release_sensor_closed", fd_closed(fsnr));
	CHECK("mi_fd_release_poll_closed", fd_closed(fpoll));
	CHECK("mi_fd_release_non_mi_kept", fd_open(fother) && fd_open(fmisc));
	/* Healthy-exit order first, other MI modules after, sys never. */
	CHECK("mi_fd_release_order_0_vif", strcmp(names[0], "vif") == 0);
	CHECK("mi_fd_release_order_1_vpe", strcmp(names[1], "vpe") == 0);
	CHECK("mi_fd_release_order_2_venc", strcmp(names[2], "venc") == 0);
	CHECK("mi_fd_release_order_3_sensor", strcmp(names[3], "sensor") == 0);
	CHECK("mi_fd_release_order_4_poll", strcmp(names[4], "poll") == 0);
	CHECK("mi_fd_release_names_end", names[5][0] == '\0');

	/* Idempotent: only sys is left, a second pass closes nothing. */
	n = mi_fd_release_non_sys_in("/proc/self/fd", dev, NULL, 0);
	CHECK("mi_fd_release_second_pass_0", n == 0);
	CHECK("mi_fd_release_second_pass_sys_kept", fd_open(fsys));

	/* names[] shorter than the fds: all are still closed, names truncated. */
	fvif = touch_open(dev, "mi_vif");
	fsnr = touch_open(dev, "mi_sensor");
	memset(names, 0, sizeof(names));
	n = mi_fd_release_non_sys_in("/proc/self/fd", dev, names, 1);
	CHECK("mi_fd_release_short_names_count", n == 2);
	CHECK("mi_fd_release_short_names_first", strcmp(names[0], "vif") == 0);
	CHECK("mi_fd_release_short_names_untouched", names[1][0] == '\0');
	CHECK("mi_fd_release_short_names_closed", fd_closed(fvif) && fd_closed(fsnr));

	close(fsys);
	close(fother);
	close(fmisc);
	{
		const char *rel[] = { "mi_sys", "mi_venc", "mi_vpe", "mi_vif",
			"mi_sensor", "mi_poll", "other", "misc" };
		char p[400];
		for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
			snprintf(p, sizeof(p), "%s/%s", dev, rel[i]);
			unlink(p);
		}
	}
	rmdir(dev);
	rmdir(root);
	return failures;
}

/* The teardown watchdog is a fork() of waybeam; without this it holds a copy
 * of every MI fd and its own exit becomes the real (PF_EXITING) release. */
static int test_close_from(void)
{
	int failures = 0;
	int a = open("/dev/null", O_RDONLY);
	int b = open("/dev/null", O_RDONLY);
	int status = -1;
	pid_t pid;

	CHECK("mi_fd_close_from_setup", a >= 3 && b > a);
	pid = fork();
	if (pid == 0) {
		int left = 0;
		mi_fd_close_from(3);
		for (int fd = 3; fd < 1024; fd++)
			left += fd_open(fd);
		_exit(left > 100 ? 100 : left);
	}
	CHECK("mi_fd_close_from_fork", pid > 0);
	if (pid > 0)
		waitpid(pid, &status, 0);
	CHECK("mi_fd_close_from_child_none_left",
		WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK("mi_fd_close_from_parent_untouched", fd_open(a) && fd_open(b));
	CHECK("mi_fd_close_from_std_kept", fd_open(0) || fd_open(1) || fd_open(2));
	close(a);
	close(b);
	return failures;
}

/* Wiring pin: the star6e teardown uses both helpers, in the right places. */
static int test_runtime_wiring(void)
{
	int failures = 0;
	FILE *f = fopen("src/star6e_runtime.c", "r");
	char *buf = NULL;
	long len = 0;

	CHECK("mi_fd_wiring_open_source", f != NULL);
	if (!f)
		return failures;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = calloc(1, (size_t)len + 1);
	if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len)
		len = 0;
	fclose(f);
	if (!buf || len == 0) {
		CHECK("mi_fd_wiring_read_source", 0);
		free(buf);
		return failures;
	}
	{	/* a CRLF checkout must match too */
		char *w = buf;
		for (const char *r = buf; *r; r++)
			if (*r != '\r')
				*w++ = *r;
		*w = '\0';
	}

	const char *td = strstr(buf, "static void star6e_runner_teardown(");
	const char *td_end = td ? strstr(td, "\n}\n") : NULL;
	const char *fork_at = td ? strstr(td, "fork()") : NULL;
	const char *child_close = td ? strstr(td, "mi_fd_close_from(3)") : NULL;
	const char *child_loop = td ? strstr(td, "for (i = 0; i < 6; i++)") : NULL;
	const char *deinit = td ? strstr(td, "star6e_mi_deinit();") : NULL;
	const char *release = td ? strstr(td, "mi_fd_release_non_sys();") : NULL;

	CHECK("mi_fd_wiring_teardown_found", td && td_end);
	CHECK("mi_fd_wiring_child_closes_fds",
		fork_at && child_close && child_loop &&
		fork_at < child_close && child_close < child_loop);
	CHECK("mi_fd_wiring_release_after_deinit",
		deinit && release && deinit < release && release < td_end);
	free(buf);
	return failures;
}

int test_mi_fd_release(void)
{
	int failures = 0;

	failures += test_module_name();
	failures += test_release_order();
	failures += test_close_from();
	failures += test_runtime_wiring();
	return failures;
}
