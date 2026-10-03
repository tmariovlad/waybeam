#ifndef MI_FD_RELEASE_H
#define MI_FD_RELEASE_H

/*
 * Exit-time release order of the MI device fds (/dev/mi_*).
 *
 * Every libmi_<mod>.so opens /dev/mi_<mod> lazily on its first API call and
 * closes it only in MI_<MOD>_DeInitDev, which waybeam never calls, so the fds
 * are released when the process (or a forked child holding copies) exits.
 *
 * mi_common.ko (air md5 1aac0ff7) keeps one global "client exiting" flag:
 *   - any /dev/mi_* release made while the releasing task is PF_EXITING sets it;
 *   - only the release of the "sys" device, also under PF_EXITING, clears it;
 *   - while it is set, every MI_DEVICE_Open sleeps 2 s.
 * A release made by a live task neither sets nor clears it.
 * See OpenIPC repos/tasks/vmode-presets-2026-09-27/13-RE-mi-device-open-2s.md.
 *
 * So a clean exit must leave no non-sys MI release for the exit path:
 * close them while alive, keep "sys" for the exit (its exit-time release
 * also clears a flag a predecessor may have left set).
 */

#define MI_FD_RELEASE_MAX 32
#define MI_FD_NAME_MAX 16

/* Module name of an MI device link target under dev_root
 * ("<dev_root>/mi_<mod>" or "<dev_root>/mi/<mod>"), or NULL. */
const char *mi_fd_module(const char *target, const char *dev_root);

/* Close every fd listed in fd_dir whose target is an MI device under dev_root,
 * except "sys": first vif, vpe, venc, sensor (the release order of every
 * healthy exit), then any other MI module in fd order.  The names closed are
 * copied in close order to names[] (up to max_names; names may be NULL).
 * Returns the number of fds closed. */
int mi_fd_release_non_sys_in(const char *fd_dir, const char *dev_root,
	char names[][MI_FD_NAME_MAX], int max_names);

/* mi_fd_release_non_sys_in("/proc/self/fd", "/dev", ...) plus one stderr
 * line with the order.  Call at the end of a clean teardown, after the last
 * MI call and after every thread that makes MI calls is gone. */
int mi_fd_release_non_sys(void);

/* Close every fd >= lowfd.  Async-signal-safe (getrlimit + close only), so it
 * can run in a child forked from a multithreaded parent. */
void mi_fd_close_from(int lowfd);

#endif
