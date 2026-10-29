/*
 * jvmdump - Dynamic Attach client for HotSpot JVMs.
 *
 * This speaks the same wire protocol as the JDK's Attach API, but without
 * needing a JDK, a helper JVM or any Java code:
 *
 *   request : "1\0" <command> "\0" <arg0> "\0" <arg1> "\0" <arg2> "\0"
 *   response: "<result code>\n" <output>
 *
 * The JVM only accepts connections from the same user, so when running as
 * root we temporarily switch the effective uid/gid to the target's.
 *
 * SPDX-License-Identifier: MIT
 */
#include "attach.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* HotSpot's attach listener limits. */
#define ATTACH_ARG_MAX 1024
#define ATTACH_REQUEST_MAX 8192

static int read_java_tmpdir(pid_t pid, char *out, size_t outsz) {
    char path[64];
    char buf[8192];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    for (size_t i = 0; i + 1 < n;) {
        const char *arg = buf + i;
        size_t len = strnlen(arg, n - i);

        if (len > 17 && strncmp(arg, "-Djava.io.tmpdir=", 17) == 0) {
            const char *val = arg + 17;
            struct stat st;
            if (*val == '/' && stat(val, &st) == 0 && S_ISDIR(st.st_mode) &&
                snprintf(out, outsz, "%s", val) < (int)outsz) {
                return 0;
            }
        }
        i += len + 1;
    }
    return -1;
}

static void add_candidate(JvmdAttachTarget *t, const char *tmpdir) {
    char path[4096];

    if (t->n_sockets >= JVMD_MAX_SOCKETS || tmpdir == NULL || *tmpdir == '\0') {
        return;
    }
    if (snprintf(path, sizeof(path), "%s/.java_pid%d", tmpdir, (int)t->pid) >= (int)sizeof(path)) {
        return;
    }
    for (int i = 0; i < t->n_sockets; i++) {
        if (strcmp(t->sockets[i], path) == 0) {
            return;
        }
    }
    snprintf(t->sockets[t->n_sockets++], sizeof(t->sockets[0]), "%s", path);
}

int jvmd_attach_prepare(JvmdAttachTarget *t, pid_t pid, char *err, size_t errlen) {
    char path[64];
    char tmp[PATH_MAX];
    char prop[PATH_MAX];
    struct stat st;
    const char *env;

    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->uid = geteuid();
    t->gid = getegid();

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    if (stat(path, &st) != 0) {
        snprintf(err, errlen, "no such process: %d", (int)pid);
        return -1;
    }
    if (jvmd_read_uids(pid, &t->uid, &t->gid) != 0) {
        snprintf(err, errlen, "cannot read credentials of process %d", (int)pid);
        return -1;
    }

    env = getenv("JVMD_TMPDIR");
    if (env != NULL && *env != '\0') {
        /* Explicit override: only look where the user told us to. */
        add_candidate(t, env);
    } else {
        /* HotSpot normally puts the socket in /tmp (it ignores java.io.tmpdir),
         * but /proc/<pid>/root/tmp is the right thing to try in containers and
         * some JVMs honour the property, so try all sensible places. */
        if (snprintf(tmp, sizeof(tmp), "/proc/%d/root/tmp", (int)pid) < (int)sizeof(tmp) &&
            stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_candidate(t, tmp);
        }
        if (read_java_tmpdir(pid, prop, sizeof(prop)) == 0) {
            add_candidate(t, prop);
        }
        add_candidate(t, "/tmp");
    }

    if (t->n_sockets == 0) {
        snprintf(err, errlen, "cannot determine the attach socket path of process %d", (int)pid);
        return -1;
    }
    return 0;
}

static int connect_unix(const char *path) {
    struct sockaddr_un sa;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Returns a connected socket or -1; last_errno receives the error of the last try. */
static int try_connect(const JvmdAttachTarget *t, int *last_errno) {
    for (int i = 0; i < t->n_sockets; i++) {
        int fd = connect_unix(t->sockets[i]);
        if (fd >= 0) {
            return fd;
        }
        if (last_errno != NULL) {
            *last_errno = errno;
        }
    }
    return -1;
}

static void append_socket_list(const JvmdAttachTarget *t, char *out, size_t outsz) {
    size_t used = 0;

    out[0] = '\0';
    for (int i = 0; i < t->n_sockets; i++) {
        int n = snprintf(out + used, outsz - used, "%s%s", i == 0 ? "" : ", ", t->sockets[i]);
        if (n < 0 || (size_t)n >= outsz - used) {
            return;
        }
        used += (size_t)n;
    }
}

/* Ask the JVM to start its attach listener: drop a .attach_pid<pid> trigger
 * file into its working directory (or /tmp) and send it SIGQUIT. */
static int start_attach_listener(const JvmdAttachTarget *t, int timeout_ms, char *err, size_t errlen) {
    char dir[PATH_MAX];
    char trigger[PATH_MAX + 64];
    const char *chosen = NULL;
    struct stat st;
    int fd;
    int waited = 0;

    static const char *const templates[] = { "/proc/%d/cwd", "/proc/%d/root/tmp", NULL };

    for (int pass = 0; pass < 3 && chosen == NULL; pass++) {
        if (templates[pass] != NULL) {
            snprintf(dir, sizeof(dir), templates[pass], (int)t->pid);
        } else {
            snprintf(dir, sizeof(dir), "%s", "/tmp");
        }
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        snprintf(trigger, sizeof(trigger), "%s/.attach_pid%d", dir, (int)t->pid);
        fd = open(trigger, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0) {
            continue;
        }
        close(fd);
        if (stat(trigger, &st) == 0 && st.st_uid == geteuid()) {
            chosen = trigger;
        } else {
            unlink(trigger);
        }
    }

    if (chosen == NULL) {
        snprintf(err, errlen, "cannot create the attach trigger file for process %d: %s", (int)t->pid,
                 strerror(errno));
        return -1;
    }

    if (kill(t->pid, SIGQUIT) != 0) {
        unlink(chosen);
        snprintf(err, errlen, "cannot signal process %d: %s", (int)t->pid, strerror(errno));
        return -1;
    }

    while (waited < timeout_ms) {
        int fd2 = try_connect(t, NULL);
        if (fd2 >= 0) {
            close(fd2);
            unlink(chosen);
            return 0;
        }
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        waited += 20;
    }

    unlink(chosen);
    snprintf(err, errlen, "the JVM did not open its attach socket within %d ms (tried %s)", timeout_ms,
             t->sockets[0]);
    return -1;
}

static int open_socket(const JvmdAttachTarget *t, int timeout_ms, char *err, size_t errlen) {
    int last_errno = 0;
    int fd = try_connect(t, &last_errno);
    char list[4096];

    if (fd >= 0) {
        return fd;
    }
    append_socket_list(t, list, sizeof(list));

    if (last_errno != ENOENT && last_errno != ECONNREFUSED) {
        snprintf(err, errlen, "cannot connect to %s: %s", list, strerror(last_errno));
        return -1;
    }
    if (!jvmd_process_has_libjvm(t->pid)) {
        snprintf(err, errlen,
                 "attach socket not found (%s) and process %d does not look like a HotSpot JVM "
                 "(no libjvm.so in its memory map)",
                 list, (int)t->pid);
        return -1;
    }
    if (start_attach_listener(t, timeout_ms, err, errlen) != 0) {
        return -1;
    }
    fd = try_connect(t, &last_errno);
    if (fd < 0) {
        snprintf(err, errlen, "cannot connect to %s: %s", list, strerror(last_errno));
    }
    return fd;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_request(int fd, const char *cmd, const char *a0, const char *a1, const char *a2,
                        char *err, size_t errlen) {
    char buf[ATTACH_REQUEST_MAX];
    const char *parts[4];
    size_t n = 0;

    parts[0] = cmd;
    parts[1] = a0 != NULL ? a0 : "";
    parts[2] = a1 != NULL ? a1 : "";
    parts[3] = a2 != NULL ? a2 : "";

    buf[n++] = '1';
    buf[n++] = '\0';
    for (int i = 0; i < 4; i++) {
        size_t len = strlen(parts[i]);
        if (len + 1 > ATTACH_ARG_MAX) {
            snprintf(err, errlen, "attach argument %d is longer than %d bytes", i, ATTACH_ARG_MAX - 1);
            return -1;
        }
        if (n + len + 1 > sizeof(buf)) {
            snprintf(err, errlen, "attach request is too long");
            return -1;
        }
        memcpy(buf + n, parts[i], len);
        n += len;
        buf[n++] = '\0';
    }
    if (write_all(fd, buf, n) != 0) {
        snprintf(err, errlen, "cannot send the attach request: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static char *read_response(int fd, size_t *out_len, char *err, size_t errlen) {
    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    struct timeval tv = { 3600, 0 };

    if (buf == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        ssize_t n;
        if (len + 4096 >= cap) {
            char *bigger = realloc(buf, cap * 2);
            if (bigger == NULL) {
                free(buf);
                snprintf(err, errlen, "out of memory");
                return NULL;
            }
            buf = bigger;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len - 1);
        if (n > 0) {
            len += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

int jvmd_attach_command(const JvmdAttachTarget *t, int timeout_ms, const char *cmd, const char *a0,
                        const char *a1, const char *a2, JvmdAttachReply *reply, char *err,
                        size_t errlen) {
    int fd;
    char *raw;
    size_t raw_len = 0;
    char *newline;

    memset(reply, 0, sizeof(*reply));

    fd = open_socket(t, timeout_ms, err, errlen);
    if (fd < 0) {
        if (err[0] == '\0') {
            snprintf(err, errlen, "cannot connect to %s: %s", t->sockets[0], strerror(errno));
        }
        return -1;
    }
    if (send_request(fd, cmd, a0, a1, a2, err, errlen) != 0) {
        close(fd);
        return -1;
    }
    raw = read_response(fd, &raw_len, err, errlen);
    close(fd);
    if (raw == NULL) {
        return -1;
    }
    if (raw_len == 0) {
        free(raw);
        snprintf(err, errlen, "the JVM closed the connection without answering");
        return -1;
    }

    newline = strchr(raw, '\n');
    if (newline != NULL) {
        *newline = '\0';
        reply->code = atoi(raw);
        reply->text = strdup(newline + 1);
    } else {
        reply->code = atoi(raw);
        reply->text = strdup("");
    }
    free(raw);
    if (reply->text == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    return 0;
}

void jvmd_attach_reply_free(JvmdAttachReply *r) {
    free(r->text);
    r->text = NULL;
    r->code = 0;
}
