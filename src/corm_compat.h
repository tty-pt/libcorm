/**
 * @file corm_compat.h
 * @brief Portability shims for the corm CLI (corm.c) and daemon (cormd.c).
 *
 * Delegates cross-platform system, socket, and I/O primitives directly to
 * libqsys across all supported platforms (POSIX and Windows).
 *
 * Internal to src/; never installed.
 */
#ifndef CORM_COMPAT_H
#define CORM_COMPAT_H

#include <stdint.h>
#include <errno.h>
#include <ttypt/qsys.h>

/* flock(2) op bits — values match QSYS_LOCK_* and <sys/file.h> */
#ifndef LOCK_SH
#define LOCK_SH QSYS_LOCK_SH
#endif
#ifndef LOCK_EX
#define LOCK_EX QSYS_LOCK_EX
#endif
#ifndef LOCK_NB
#define LOCK_NB QSYS_LOCK_NB
#endif
#ifndef LOCK_UN
#define LOCK_UN QSYS_LOCK_UN
#endif

/* ── Winsock / Socket lifecycle ───────────────────────────────────────── */

#define corm_sock_init() qsys_sock_init()
#define corm_sock_close(fd) qsys_sock_close((qsys_socket_t)(fd))
#define corm_sock_read(fd, b, n) qsys_sock_recv((qsys_socket_t)(fd), (b), (n), 0)
#define corm_sock_write(fd, b, n) qsys_sock_send((qsys_socket_t)(fd), (b), (n), 0)
#define corm_sock_nonblock(fd) qsys_sock_set_nonblock((qsys_socket_t)(fd), 1)
#define corm_sock_wouldblock() qsys_sock_wouldblock()
#define corm_sock_err() qsys_sock_strerror()

/* ── Advisory file locks ──────────────────────────────────────────────── */

#define corm_flock(fd, op) qsys_flock((fd), (op))

/* ── File system & libc gaps ──────────────────────────────────────────── */

#define corm_ftruncate(fd, n) qsys_ftruncate((fd), (n))
#define corm_dprintf(fd, ...) qsys_dprintf((fd), __VA_ARGS__)
#define corm_mkdir(p) qsys_mkdir(p)
#define corm_realpath(path, out, len) qsys_realpath((path), (out))

/* ── Time, process, poll, signals ─────────────────────────────────────── */

#define corm_now_ms() qsys_now_ms()
#define corm_sleep_ms(ms) qsys_sleep_ms(ms)
#define corm_pid_alive(pid) qsys_pid_alive(pid)
#define corm_poll(fds, nfds, timeout) qsys_poll((fds), (nfds_t)(nfds), (timeout))
#define corm_stop_on_signal(h) qsys_stop_on_signal(h)
#define corm_ignore_pipe() qsys_ignore_pipe()
#define corm_self_rss_bytes() qsys_self_rss_bytes()

#endif /* CORM_COMPAT_H */
