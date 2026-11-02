/**
 * @file corm_compat.h
 * @brief Portability shims for the corm CLI (corm.c) and daemon (cormd.c).
 *
 * Delegates cross-platform system, socket, and I/O primitives directly to
 * libstele (stl_) across all supported platforms (POSIX and Windows).
 *
 * Internal to src/; never installed.
 */
#ifndef CORM_COMPAT_H
#define CORM_COMPAT_H

#include <stdint.h>
#include <errno.h>
#include <ttypt/stele.h>

/* flock(2) op bits — values match STL_LOCK_* and <sys/file.h> */
#ifndef LOCK_SH
#define LOCK_SH STL_LOCK_SH
#endif
#ifndef LOCK_EX
#define LOCK_EX STL_LOCK_EX
#endif
#ifndef LOCK_NB
#define LOCK_NB STL_LOCK_NB
#endif
#ifndef LOCK_UN
#define LOCK_UN STL_LOCK_UN
#endif

#define corm_sock_init() stl_sock_init()
#define corm_sock_close(fd) stl_sock_close((stl_socket_t)(fd))
#define corm_sock_read(fd, b, n) stl_sock_recv((stl_socket_t)(fd), (b), (n), 0)
#define corm_sock_write(fd, b, n) stl_sock_send((stl_socket_t)(fd), (b), (n), 0)
#define corm_sock_nonblock(fd) stl_sock_set_nonblock((stl_socket_t)(fd), 1)
#define corm_sock_wouldblock() stl_sock_wouldblock()
#define corm_sock_err() stl_sock_strerror()

#define corm_flock(fd, op) stl_flock((fd), (op))
#define corm_ftruncate(fd, n) stl_ftruncate((fd), (n))
#define corm_dprintf(fd, ...) stl_dprintf((fd), __VA_ARGS__)
#define corm_mkdir(p) stl_mkdir(p)
#define corm_realpath(path, out, len) stl_realpath((path), (out))

#define corm_now_ms() stl_now_ms()
#define corm_sleep_ms(ms) stl_sleep_ms(ms)
#define corm_pid_alive(pid) stl_pid_alive(pid)
#define corm_poll(fds, nfds, timeout) stl_poll((fds), (nfds_t)(nfds), (timeout))
#define corm_stop_on_signal(h) stl_stop_on_signal(h)
#define corm_ignore_pipe() stl_ignore_pipe()
#define corm_self_rss_bytes() stl_self_rss_bytes()

#endif /* CORM_COMPAT_H */
