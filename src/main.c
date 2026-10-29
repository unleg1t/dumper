/*
 * jvmdump - dump the class files of a live HotSpot JVM.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "attach.h"
#include "common.h"
#include "util.h"

static const char *prog = JVMD_NAME;

static void usage(FILE *out) {
    fprintf(out,
            "Usage: %s [options] <pid>\n"
            "\n"
            "Dump every class file loaded by a live HotSpot JVM using JVMTI.\n"
            "No Java agent, no JDK, no attach API helper: just one native binary.\n"
            "\n"
            "Options:\n"
            "  -o, --out DIR         Output directory\n"
            "                        (default: ./jvmdump-<pid>-<timestamp>, or /tmp/...\n"
            "                         when dumping another user's JVM)\n"
            "  -i, --include GLOB    Only dump classes matching GLOB, e.g. 'com.example.*'\n"
            "                        Can be repeated or given as a comma separated list.\n"
            "  -e, --exclude GLOB    Skip classes matching GLOB (repeatable)\n"
            "      --no-jdk          Shorthand for excluding java.*, javax.*, jdk.*, sun.* ...\n"
            "  -l, --list            Only list class names (writes <out>/classes.txt)\n"
            "  -f, --flat            Write ClassName.class instead of ClassName/ trees\n"
            "  -a, --agent PATH      Path to libjvmdump.so (auto-detected by default)\n"
            "      --timeout SEC     Seconds to wait for the attach listener (default 10)\n"
            "  -q, --quiet           Only print the output path (handy for scripting)\n"
            "  -v, --verbose         Print agent diagnostics to stderr\n"
            "  -V, --version         Print version and exit\n"
            "  -h, --help            Print this help and exit\n"
            "\n"
            "Examples:\n"
            "  %s 1234\n"
            "  %s --no-jdk 1234\n"
            "  %s -i 'com.example.*' -o /tmp/dump 1234\n"
            "  %s --list 1234 | grep -i mypackage\n"
            "\n"
            "The target JVM must allow dynamic attach. On newer JDKs that were\n"
            "started with -XX:-EnableDynamicAgentLoading (or with a hardened\n"
            "default) add -XX:+EnableDynamicAgentLoading to its command line.\n",
            prog, prog, prog, prog, prog);
}

static int parse_filters(JvmdStrVec *v, const char *arg) {
    char *copy = strdup(arg);
    char *save = NULL;
    int count = 0;

    if (copy == NULL) {
        return -1;
    }
    for (char *tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        char *end;
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (*tok == '\0') {
            continue;
        }
        if (jvmd_sv_push(v, tok) != 0) {
            free(copy);
            return -1;
        }
        count++;
    }
    free(copy);
    return count;
}

static int absolute_path(const char *in, char *out, size_t outsz) {
    char cwd[PATH_MAX];

    if (in == NULL || *in == '\0') {
        return -1;
    }
    if (in[0] == '/') {
        if (snprintf(out, outsz, "%s", in) >= (int)outsz) {
            return -1;
        }
    } else {
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return -1;
        }
        if (snprintf(out, outsz, "%s/%s", cwd, in) >= (int)outsz) {
            return -1;
        }
    }
    /* strip trailing slashes */
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return 0;
}

static int executable_dir(char *out, size_t outsz) {
    char link[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", link, sizeof(link) - 1);
    char *slash;

    if (len <= 0) {
        return -1;
    }
    link[len] = '\0';
    slash = strrchr(link, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if (snprintf(out, outsz, "%s", link) >= (int)outsz) {
        return -1;
    }
    return 0;
}

static const char *find_agent(const char *explicit_path) {
    static char resolved[PATH_MAX];
    static char beside[PATH_MAX + 64];
    static char libdir[PATH_MAX + 64];
    static char builddir[PATH_MAX + 64];
    char exec_dir[PATH_MAX];
    const char *candidates[8];
    int n = 0;

    beside[0] = libdir[0] = builddir[0] = '\0';
    if (executable_dir(exec_dir, sizeof(exec_dir)) == 0) {
        snprintf(beside, sizeof(beside), "%s/%s", exec_dir, JVMD_AGENT_LIBNAME);
        snprintf(libdir, sizeof(libdir), "%s/../lib/jvmdump/%s", exec_dir, JVMD_AGENT_LIBNAME);
        snprintf(builddir, sizeof(builddir), "%s/../lib/%s", exec_dir, JVMD_AGENT_LIBNAME);
    }

    candidates[n++] = explicit_path;
    candidates[n++] = getenv("JVMD_AGENT");
    candidates[n++] = beside[0] != '\0' ? beside : NULL;
    candidates[n++] = libdir[0] != '\0' ? libdir : NULL;
    candidates[n++] = builddir[0] != '\0' ? builddir : NULL;
    candidates[n++] = JVMD_INSTALL_AGENT;

    for (int i = 0; i < n; i++) {
        if (candidates[i] == NULL || *candidates[i] == '\0') {
            continue;
        }
        if (access(candidates[i], R_OK) == 0) {
            if (realpath(candidates[i], resolved) != NULL) {
                return resolved;
            }
            return candidates[i];
        }
    }
    return NULL;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    char *buf;
    long size;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || size > (16 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void print_file(const char *path) {
    char buf[8192];
    size_t n;
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
}

static void print_reply_text(const char *text) {
    char *copy;
    char *save = NULL;

    if (text == NULL || *text == '\0') {
        return;
    }
    copy = strdup(text);
    if (copy == NULL) {
        return;
    }
    for (char *line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (strcmp(line, "return code: 0") == 0) {
            continue;
        }
        fprintf(stderr, "%s: jvm: %s\n", prog, line);
    }
    free(copy);
}

static int dump_log_value(const char *log, const char *key, unsigned long *value) {
    const char *p = log;
    size_t klen = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        if ((p == log || p[-1] == '\n') && p[klen] == ':' && p[klen + 1] == ' ') {
            char *end = NULL;
            unsigned long parsed = strtoul(p + klen + 2, &end, 10);
            if (end != p + klen + 2) {
                *value = parsed;
                return 0;
            }
        }
        p += klen;
    }
    return -1;
}

static void human_size(unsigned long bytes, char *out, size_t outsz) {
    static const char *units[] = { "B", "KiB", "MiB", "GiB" };
    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(out, outsz, "%lu B", bytes);
    } else {
        snprintf(out, outsz, "%.1f %s", value, units[unit]);
    }
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        { "out", required_argument, NULL, 'o' },
        { "include", required_argument, NULL, 'i' },
        { "exclude", required_argument, NULL, 'e' },
        { "no-jdk", no_argument, NULL, 1000 },
        { "list", no_argument, NULL, 'l' },
        { "flat", no_argument, NULL, 'f' },
        { "agent", required_argument, NULL, 'a' },
        { "timeout", required_argument, NULL, 1001 },
        { "quiet", no_argument, NULL, 'q' },
        { "verbose", no_argument, NULL, 'v' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *explicit_out = NULL;
    const char *agent_option = NULL;
    JvmdStrVec includes = { 0 };
    JvmdStrVec excludes = { 0 };
    int list_only = 0;
    int flat = 0;
    int quiet = 0;
    int verbose = 0;
    int no_jdk = 0;
    int timeout = 10;
    int is_root = (geteuid() == 0);
    int option;
    int rc = 1;
    char err[1024];
    char out_abs[PATH_MAX];
    char options[JVMD_MAX_OPTIONS];
    char logpath[PATH_MAX];
    char *log = NULL;

    while ((option = getopt_long(argc, argv, "o:i:e:lfa:qvhV", long_options, NULL)) != -1) {
        switch (option) {
        case 'o':
            explicit_out = optarg;
            break;
        case 'i':
            if (parse_filters(&includes, optarg) < 0) {
                fprintf(stderr, "%s: invalid --include value\n", prog);
                goto cleanup;
            }
            break;
        case 'e':
            if (parse_filters(&excludes, optarg) < 0) {
                fprintf(stderr, "%s: invalid --exclude value\n", prog);
                goto cleanup;
            }
            break;
        case 'l':
            list_only = 1;
            break;
        case 'f':
            flat = 1;
            break;
        case 'a':
            agent_option = optarg;
            break;
        case 'q':
            quiet = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'V':
            printf("%s %s\n", JVMD_NAME, JVMD_AGENT_VERSION);
            rc = 0;
            goto cleanup;
        case 'h':
            usage(stdout);
            rc = 0;
            goto cleanup;
        case 1000:
            no_jdk = 1;
            break;
        case 1001:
            timeout = atoi(optarg);
            if (timeout <= 0 || timeout > 600) {
                fprintf(stderr, "%s: --timeout must be between 1 and 600 seconds\n", prog);
                goto cleanup;
            }
            break;
        default:
            usage(stderr);
            rc = 2;
            goto cleanup;
        }
    }

    if (optind != argc - 1) {
        usage(stderr);
        rc = 2;
        goto cleanup;
    }

    char *end = NULL;
    errno = 0;
    long pid = strtol(argv[optind], &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || pid <= 0 || pid > INT_MAX) {
        fprintf(stderr, "%s: '%s' is not a valid pid\n", prog, argv[optind]);
        rc = 2;
        goto cleanup;
    }

    JvmdAttachTarget target;
    err[0] = '\0';
    if (jvmd_attach_prepare(&target, (pid_t)pid, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", prog, err);
        goto cleanup;
    }

    if (is_root && (target.uid != 0 || target.gid != 0)) {
        if (setegid(target.gid) != 0 || seteuid(target.uid) != 0) {
            fprintf(stderr, "%s: cannot switch to the JVM's user (uid %u, gid %u): %s\n", prog,
                    (unsigned)target.uid, (unsigned)target.gid, strerror(errno));
            goto cleanup;
        }
    }

    char default_out[PATH_MAX];
    if (explicit_out == NULL) {
        char stamp[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        if (is_root && target.uid != 0) {
            snprintf(default_out, sizeof(default_out), "/tmp/jvmdump-%ld-%s", pid, stamp);
        } else {
            snprintf(default_out, sizeof(default_out), "jvmdump-%ld-%s", pid, stamp);
        }
        explicit_out = default_out;
    }
    if (strchr(explicit_out, ',') != NULL) {
        fprintf(stderr, "%s: the output directory must not contain a comma\n", prog);
        goto cleanup;
    }
    if (absolute_path(explicit_out, out_abs, sizeof(out_abs)) != 0) {
        fprintf(stderr, "%s: invalid output directory '%s'\n", prog, explicit_out);
        goto cleanup;
    }

    const char *agent_path = find_agent(agent_option);
    if (agent_path == NULL) {
        fprintf(stderr,
                "%s: cannot find %s\n"
                "  build it with 'make' or point at it with --agent /path/to/%s\n",
                prog, JVMD_AGENT_LIBNAME, JVMD_AGENT_LIBNAME);
        goto cleanup;
    }
    if (access(agent_path, R_OK) != 0) {
        fprintf(stderr, "%s: cannot read %s: %s\n", prog, agent_path, strerror(errno));
        goto cleanup;
    }

    size_t used = 0;
    int written = snprintf(options, sizeof(options), "out=%s,list=%d,flat=%d", out_abs, list_only, flat);
    if (written < 0 || (size_t)written >= sizeof(options)) {
        fprintf(stderr, "%s: output path is too long\n", prog);
        goto cleanup;
    }
    used = (size_t)written;

    if (no_jdk) {
        static const char *jdk_globs[] = { "java.*", "javax.*", "jdk.*", "sun.*", "com.sun.*",
                                           "org.w3c.*", "org.xml.*" };
        for (size_t i = 0; i < sizeof(jdk_globs) / sizeof(jdk_globs[0]); i++) {
            written = snprintf(options + used, sizeof(options) - used, ",exclude=%s", jdk_globs[i]);
            if (written < 0 || (size_t)written >= sizeof(options) - used) {
                fprintf(stderr, "%s: too many filters\n", prog);
                goto cleanup;
            }
            used += (size_t)written;
        }
    }
    for (size_t i = 0; i < includes.len; i++) {
        written = snprintf(options + used, sizeof(options) - used, ",include=%s", includes.items[i]);
        if (written < 0 || (size_t)written >= sizeof(options) - used) {
            fprintf(stderr, "%s: too many or too long --include filters\n", prog);
            goto cleanup;
        }
        used += (size_t)written;
    }
    for (size_t i = 0; i < excludes.len; i++) {
        written = snprintf(options + used, sizeof(options) - used, ",exclude=%s", excludes.items[i]);
        if (written < 0 || (size_t)written >= sizeof(options) - used) {
            fprintf(stderr, "%s: too many or too long --exclude filters\n", prog);
            goto cleanup;
        }
        used += (size_t)written;
    }
    if (verbose) {
        written = snprintf(options + used, sizeof(options) - used, ",verbose=1");
        if (written > 0 && (size_t)written < sizeof(options) - used) {
            used += (size_t)written;
        }
    }

    if (!quiet) {
        fprintf(stderr, "%s: attaching to pid %ld (%s)\n", prog, pid, agent_path);
        if (!list_only) {
            fprintf(stderr, "%s: dumping classes to %s\n", prog, out_abs);
        }
    }

    JvmdAttachReply reply;
    memset(&reply, 0, sizeof(reply));
    if (jvmd_attach_command(&target, timeout * 1000, "load", agent_path, "true", options, &reply, err,
                            sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", prog, err);
        goto cleanup;
    }
    if (reply.code != 0) {
        fprintf(stderr, "%s: the JVM rejected the request (code %d)\n", prog, reply.code);
    }
    print_reply_text(reply.text);
    if (reply.text != NULL && strstr(reply.text, "Dynamic agent loading is not enabled") != NULL) {
        fprintf(stderr,
                "%s: hint: restart the JVM with -XX:+EnableDynamicAgentLoading, or use\n"
                "  -agentpath:%s=out=%s at startup instead\n",
                prog, agent_path, out_abs);
    }
    jvmd_attach_reply_free(&reply);

    if (snprintf(logpath, sizeof(logpath), "%s/dump.log", out_abs) >= (int)sizeof(logpath)) {
        fprintf(stderr, "%s: path is too long\n", prog);
        goto cleanup;
    }
    log = read_file(logpath);
    if (log == NULL) {
        fprintf(stderr,
                "%s: the agent did not write %s\n"
                "  * if the target JVM runs as another user, make sure %s is writable by it\n"
                "  * check the JVM's own stderr for messages from %s\n",
                prog, logpath, out_abs, JVMD_NAME);
        goto cleanup;
    }

    if (strstr(log, "status: ok") == NULL) {
        fprintf(stderr, "%s: the dump did not complete\n", prog);
        fputs(log, stderr);
        goto cleanup;
    }

    if (list_only) {
        unsigned long listed = 0;
        char listpath[PATH_MAX];

        dump_log_value(log, "classes-selected", &listed);
        snprintf(listpath, sizeof(listpath), "%s/classes.txt", out_abs);
        if (quiet) {
            printf("%s\n", listpath);
        } else {
            print_file(listpath);
            fprintf(stderr, "%s: %lu classes listed in %s\n", prog, listed, listpath);
        }
    } else {
        unsigned long count = 0;
        unsigned long bytes = 0;
        unsigned long failed = 0;
        unsigned long loaded = 0;
        char pretty[64];

        dump_log_value(log, "files-written", &count);
        dump_log_value(log, "bytes-written", &bytes);
        dump_log_value(log, "failed-count", &failed);
        dump_log_value(log, "classes-loaded", &loaded);
        human_size(bytes, pretty, sizeof(pretty));

        if (verbose) {
            fprintf(stderr, "---- %s ----\n", logpath);
            fputs(log, stderr);
            fprintf(stderr, "----\n");
        } else if (!quiet) {
            fprintf(stderr, "%s: wrote %lu class files (%s) from %lu loaded classes, %lu failed\n",
                    prog, count, pretty, loaded, failed);
        }
        printf("%s\n", out_abs);
        if (failed > 0 && !quiet) {
            fprintf(stderr, "%s: see %s/failed.txt for the classes that could not be dumped\n", prog,
                    out_abs);
        }
    }

    rc = 0;

cleanup:
    jvmd_sv_free(&includes);
    jvmd_sv_free(&excludes);
    return rc;
}
