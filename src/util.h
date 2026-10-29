/*
 * jvmdump - shared helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JVMD_UTIL_H
#define JVMD_UTIL_H

#include <stddef.h>
#include <sys/types.h>

/* mkdir -p. Returns 0 on success, -1 on error (errno set). */
int jvmd_mkdir_p(const char *path, mode_t mode);

/* Shell style glob match supporting '*' and '?'. */
int jvmd_glob_match(const char *pattern, const char *text);

/* Read the effective uid/gid of a process from /proc/<pid>/status. */
int jvmd_read_uids(pid_t pid, uid_t *uid, gid_t *gid);

/* True when /proc/<pid>/maps contains libjvm.so. */
int jvmd_process_has_libjvm(pid_t pid);

/* Monotonic clock in seconds, used for timings only. */
double jvmd_now(void);

/* Minimal growable vector of owned strings. */
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} JvmdStrVec;

int jvmd_sv_push(JvmdStrVec *v, const char *s);
void jvmd_sv_sort(JvmdStrVec *v);
void jvmd_sv_free(JvmdStrVec *v);

#endif
