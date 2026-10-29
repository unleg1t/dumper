/*
 * jvmdump - Dynamic Attach client for HotSpot JVMs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JVMD_ATTACH_H
#define JVMD_ATTACH_H

#include <stddef.h>
#include <sys/types.h>

#define JVMD_MAX_SOCKETS 4

typedef struct {
    pid_t pid;
    uid_t uid;
    gid_t gid;
    /* Candidate attach sockets; HotSpot normally uses /tmp/.java_pid<pid>,
     * containers may need /proc/<pid>/root/tmp/.java_pid<pid>. */
    char sockets[JVMD_MAX_SOCKETS][4096];
    int n_sockets;
} JvmdAttachTarget;

typedef struct {
    int code;     /* operation result code reported by HotSpot */
    char *text;   /* output that followed the result line */
} JvmdAttachReply;

/* Fill in the attach target, including candidate socket paths and the JVM's uid/gid. */
int jvmd_attach_prepare(JvmdAttachTarget *t, pid_t pid, char *err, size_t errlen);

/* Connect, send one request and collect the reply.  Returns 0 on success.
 * timeout_ms bounds how long we wait for the JVM to open its attach socket. */
int jvmd_attach_command(const JvmdAttachTarget *t, int timeout_ms, const char *cmd, const char *a0,
                        const char *a1, const char *a2, JvmdAttachReply *reply, char *err,
                        size_t errlen);

void jvmd_attach_reply_free(JvmdAttachReply *r);

#endif
