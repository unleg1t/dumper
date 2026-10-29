/*
 * jvmdump - JVMTI agent that dumps every loaded class file of a live JVM.
 *
 * Two entry points are provided:
 *   Agent_OnAttach : used by the jvmdump client (dynamic attach)
 *   Agent_OnLoad   : usable via -agentpath:...=out=/tmp/dump
 *
 * Loaded classes are enumerated with GetLoadedClasses() and their class file
 * bytes are obtained by retransforming them while a ClassFileLoadHook callback
 * is installed.  Nothing is ever redefined (the callback leaves the class file
 * data untouched), so the running application keeps behaving exactly as before
 * apart from the usual deoptimisation that a retransformation implies.
 *
 * SPDX-License-Identifier: MIT
 */
#include <jni.h>
#include <jvmti.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "util.h"

#define MAX_FILTERS 64
#define BATCH_SIZE 32
#define MAX_CLASS_NAME 4096
#define MAX_DIR_BUF (PATH_MAX + 64)
#define NAME_BUCKETS 8192
#define HASHES_PER_NAME 4

typedef struct NameEntry {
    char *name;
    uint64_t hashes[HASHES_PER_NAME];
    int nhash;
    int count;
    struct NameEntry *next;
} NameEntry;

static struct {
    JavaVM *vm;
    jvmtiEnv *jvmti;

    char outdir[PATH_MAX];
    char logpath[PATH_MAX];
    FILE *log;
    FILE *manifest;
    char last_dir[MAX_DIR_BUF];

    int list_only;
    int flat;
    int verbose;
    int delay_seconds;
    volatile int dumping;

    char *include[MAX_FILTERS];
    int n_include;
    char *exclude[MAX_FILTERS];
    int n_exclude;

    unsigned long loaded;
    unsigned long selected;
    unsigned long filtered;
    unsigned long written;
    unsigned long bytes;
    unsigned long duplicates;
    unsigned long unnamed;
    unsigned long failed;

    JvmdStrVec failed_names;
    NameEntry *names[NAME_BUCKETS];
} g;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_startup_options;

/*
 * JDK 21 and later print
 *
 *   WARNING: A JVMTI agent has been loaded dynamically (<path>)
 *   WARNING: If a serviceability tool is in use, please run with ...
 *   WARNING: Dynamic loading of agents will be disallowed by default ...
 *
 * to the target's stderr.  HotSpot writes those lines after dlopen()ing the
 * library but before Agent_OnAttach() is invoked, so no code of ours can run
 * early enough to keep them from the application's console, and the flag that
 * disables them (-XX:+EnableDynamicAgentLoading) cannot be turned on at
 * runtime.  The only remaining lever is the target's stderr descriptor: point
 * it at /dev/null while the loader hands control to us and restore it as the
 * first thing in the entry point.  The window is a few microseconds and a
 * watchdog restores the descriptor even if the entry point is never called.
 *
 * This is done only when the library is loaded by HotSpot's attach listener
 * (the only case in which the warning is printed); with -agentpath: at startup
 * stderr is left completely alone.
 */
static pthread_mutex_t g_stderr_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_stderr_saved = -1;

static int loaded_by_attach_listener(void) {
    char comm[64] = "";
    FILE *f = fopen("/proc/thread-self/comm", "r");
    int match = 0;

    if (f == NULL) {
        return 0;
    }
    if (fgets(comm, sizeof(comm), f) != NULL) {
        comm[strcspn(comm, "\n")] = '\0';
        match = strcmp(comm, "Attach Listener") == 0;
    }
    fclose(f);
    return match;
}

static void restore_stderr(void) {
    pthread_mutex_lock(&g_stderr_lock);
    if (g_stderr_saved >= 0) {
        dup2(g_stderr_saved, STDERR_FILENO);
        close(g_stderr_saved);
        g_stderr_saved = -1;
    }
    pthread_mutex_unlock(&g_stderr_lock);
}

static void *stderr_watchdog(void *arg) {
    struct timespec ts = { 2, 0 };

    (void)arg;
    nanosleep(&ts, NULL);
    restore_stderr();
    return NULL;
}

__attribute__((constructor)) static void mute_stderr_until_entry(void) {
    pthread_attr_t attr;
    pthread_t watchdog;
    int saved;
    int nullfd;

    /* Leave unrelated processes alone (e.g. if someone dlopen()s the agent by
     * accident) and tolerate a closed stderr. */
    if (!loaded_by_attach_listener()) {
        return;
    }
    saved = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 10);
    if (saved < 0) {
        return;
    }
    nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullfd < 0) {
        close(saved);
        return;
    }
    pthread_mutex_lock(&g_stderr_lock);
    if (dup2(nullfd, STDERR_FILENO) == STDERR_FILENO) {
        g_stderr_saved = saved;
        saved = -1;
    }
    pthread_mutex_unlock(&g_stderr_lock);
    close(nullfd);
    if (saved >= 0) {
        close(saved);
        return;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&watchdog, &attr, stderr_watchdog, NULL) != 0) {
        /* The entry point still restores stderr in the normal case. */
    }
    pthread_attr_destroy(&attr);
}

static void jlog(const char *fmt, ...) {
    va_list ap;

    if (g.log != NULL) {
        va_start(ap, fmt);
        vfprintf(g.log, fmt, ap);
        va_end(ap);
        fputc('\n', g.log);
        fflush(g.log);
    }
    if (g.verbose) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

static int parse_options(const char *options) {
    char *copy;
    char *save = NULL;

    if (options == NULL || *options == '\0') {
        return 0;
    }
    copy = strdup(options);
    if (copy == NULL) {
        return -1;
    }

    for (char *tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        char *key = tok;
        char *val = NULL;
        char *eq = strchr(tok, '=');

        if (eq != NULL) {
            *eq = '\0';
            val = eq + 1;
        }
        if (strcmp(key, "out") == 0) {
            if (val == NULL || *val == '\0' || strlen(val) >= sizeof(g.outdir)) {
                free(copy);
                return -1;
            }
            strcpy(g.outdir, val);
        } else if (strcmp(key, "list") == 0) {
            g.list_only = (val != NULL && strcmp(val, "1") == 0);
        } else if (strcmp(key, "flat") == 0) {
            g.flat = (val != NULL && strcmp(val, "1") == 0);
        } else if (strcmp(key, "verbose") == 0) {
            g.verbose = (val != NULL && strcmp(val, "1") == 0);
        } else if (strcmp(key, "include") == 0 || strcmp(key, "exclude") == 0) {
            int is_include = (strcmp(key, "include") == 0);

            if (val == NULL || *val == '\0') {
                free(copy);
                return -1;
            }
            if (is_include && g.n_include < MAX_FILTERS) {
                g.include[g.n_include++] = strdup(val);
            } else if (!is_include && g.n_exclude < MAX_FILTERS) {
                g.exclude[g.n_exclude++] = strdup(val);
            } else {
                free(copy);
                return -1;
            }
        }
        /* unknown keys are ignored on purpose */
    }
    free(copy);
    return 0;
}

static void sanitize_char(char c, int flat, int for_path, char *out) {
    unsigned char u = (unsigned char)c;

    if (c == '/') {
        *out = for_path ? (flat ? '.' : '/') : '.';
    } else if (isalnum(u) || c == '_' || c == '$' || c == '-' || c == '.') {
        *out = c;
    } else {
        *out = '_';
    }
}

/* com/example/Foo -> com.example.Foo */
static int internal_to_dotted(const char *internal, char *out, size_t outsz) {
    size_t j = 0;

    for (size_t i = 0; internal[i] != '\0'; i++) {
        char c;
        if (j + 2 >= outsz) {
            return -1;
        }
        sanitize_char(internal[i], 0, 0, &c);
        out[j++] = c;
    }
    out[j] = '\0';
    return j > 0 ? 0 : -1;
}

/* com/example/Foo -> com/example/Foo.class (or com.example.Foo.class when flat) */
static int internal_to_relpath(const char *internal, int flat, char *out, size_t outsz) {
    size_t j = 0;

    for (size_t i = 0; internal[i] != '\0'; i++) {
        char c;
        if (j + 8 >= outsz) {
            return -1;
        }
        sanitize_char(internal[i], flat, 1, &c);
        out[j++] = c;
    }
    if (j == 0) {
        return -1;
    }
    memcpy(out + j, ".class", 7);
    j += 6;
    if (out[0] == '/' || strstr(out, "..") != NULL || strstr(out, "//") != NULL) {
        return -1;
    }
    out[j] = '\0';
    return 0;
}

static int class_selected(const char *dotted) {
    int ok = (g.n_include == 0);

    for (int i = 0; i < g.n_include && !ok; i++) {
        if (jvmd_glob_match(g.include[i], dotted)) {
            ok = 1;
        }
    }
    if (!ok) {
        return 0;
    }
    for (int i = 0; i < g.n_exclude; i++) {
        if (jvmd_glob_match(g.exclude[i], dotted)) {
            return 0;
        }
    }
    return 1;
}

static uint64_t hash_class_data(const unsigned char *data, jint len) {
    uint64_t h = 1469598103934665603ULL;
    for (jint i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)(uint32_t)len;
    h *= 1099511628211ULL;
    return h;
}

static unsigned name_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h % NAME_BUCKETS;
}

static NameEntry *name_lookup(const char *name) {
    unsigned h = name_hash(name);
    for (NameEntry *e = g.names[h]; e != NULL; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

static NameEntry *name_intern(const char *name) {
    unsigned h = name_hash(name);
    NameEntry *e = name_lookup(name);

    if (e != NULL) {
        return e;
    }
    e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->name = strdup(name);
    if (e->name == NULL) {
        free(e);
        return NULL;
    }
    e->next = g.names[h];
    g.names[h] = e;
    return e;
}

static void name_table_free(void) {
    for (size_t i = 0; i < NAME_BUCKETS; i++) {
        NameEntry *e = g.names[i];
        while (e != NULL) {
            NameEntry *next = e->next;
            free(e->name);
            free(e);
            e = next;
        }
        g.names[i] = NULL;
    }
}

static int write_class_file(const char *dotted, const char *rel, int index,
                            const unsigned char *data, jint len) {
    char path[MAX_DIR_BUF];
    char dir[MAX_DIR_BUF];
    char *slash;
    int written;
    FILE *f;

    if (snprintf(path, sizeof(path), "%s/classes/%s", g.outdir, rel) >= (int)sizeof(path)) {
        return -1;
    }
    if (index > 0) {
        size_t rel_len = strlen(rel);
        if (rel_len <= 6) {
            return -1;
        }
        if (snprintf(path, sizeof(path), "%s/classes/%.*s__dup%d.class", g.outdir,
                     (int)(rel_len - 6), rel, index + 1) >= (int)sizeof(path)) {
            return -1;
        }
    }

    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) {
        return -1;
    }
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if (strcmp(dir, g.last_dir) != 0) {
        if (jvmd_mkdir_p(dir, 0755) != 0) {
            return -1;
        }
        snprintf(g.last_dir, sizeof(g.last_dir), "%s", dir);
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    written = (fwrite(data, 1, (size_t)len, f) == (size_t)len);
    if (fclose(f) != 0) {
        written = 0;
    }
    if (!written) {
        unlink(path);
        return -1;
    }

    g.written++;
    g.bytes += (unsigned long)len;
    if (g.manifest != NULL) {
        fprintf(g.manifest, "%s\t%d\t%s\n", dotted, (int)len, path + strlen(g.outdir) + 1);
        fflush(g.manifest);
    }
    return 0;
}

/* Called for every class that is (re)transformed while dumping is active. */
static void JNICALL on_class_file_load_hook(jvmtiEnv *jvmti, JNIEnv *env, jclass cls, jobject loader,
                                            const char *name, jobject protection_domain,
                                            jint class_data_len, const unsigned char *class_data,
                                            jint *new_class_data_len, unsigned char **new_class_data) {
    char dotted[MAX_CLASS_NAME];
    char rel[MAX_CLASS_NAME + 16];
    uint64_t hash;
    NameEntry *entry;
    int index;
    int known = 0;

    (void)jvmti;
    (void)env;
    (void)cls;
    (void)loader;
    (void)protection_domain;
    (void)new_class_data_len;
    (void)new_class_data;

    if (!g.dumping) {
        return;
    }
    if (name == NULL || *name == '\0') {
        pthread_mutex_lock(&g_lock);
        g.unnamed++;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (class_data == NULL || class_data_len <= 0) {
        return;
    }
    if (internal_to_dotted(name, dotted, sizeof(dotted)) != 0 || !class_selected(dotted)) {
        return;
    }
    if (internal_to_relpath(name, g.flat, rel, sizeof(rel)) != 0) {
        pthread_mutex_lock(&g_lock);
        g.failed++;
        jvmd_sv_push(&g.failed_names, dotted);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    hash = hash_class_data(class_data, class_data_len);

    pthread_mutex_lock(&g_lock);
    entry = name_intern(name);
    if (entry == NULL) {
        g.failed++;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    for (int i = 0; i < entry->nhash; i++) {
        if (entry->hashes[i] == hash) {
            known = 1;
            break;
        }
    }
    if (known) {
        /* The same class was already dumped (e.g. a retried batch). */
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (entry->nhash < HASHES_PER_NAME) {
        entry->hashes[entry->nhash++] = hash;
    }
    index = entry->count++;

    if (index > 0) {
        g.duplicates++;
    }
    if (write_class_file(dotted, rel, index, class_data, class_data_len) != 0) {
        g.failed++;
        jvmd_sv_push(&g.failed_names, dotted);
    }
    pthread_mutex_unlock(&g_lock);
}

static jint get_jvmti_env(JavaVM *vm, jvmtiEnv **env) {
    static const jint versions[] = { JVMTI_VERSION_1_2, JVMTI_VERSION_1_1, JVMTI_VERSION_1_0 };

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        if ((*vm)->GetEnv(vm, (void **)env, versions[i]) == JNI_OK) {
            return JNI_OK;
        }
    }
    return JNI_ERR;
}

/* jvmtiCapabilities has grown over the years.  Always hand the JVM a
 * zero-filled buffer that is larger than any known version of the struct so
 * that newer JVMs cannot read (AddCapabilities) or write (GetCapabilities)
 * past the end of it. */
typedef union {
    jvmtiCapabilities caps;
    unsigned char pad[1024];
} CapabilityBuffer;

static void add_capability(jvmtiEnv *jvmti, CapabilityBuffer *buf, const char *what) {
    jvmtiError err = (*jvmti)->AddCapabilities(jvmti, &buf->caps);

    if (err != JVMTI_ERROR_NONE) {
        jlog("warning: cannot enable %s (jvmti error %d)", what, (int)err);
    }
}

static int enable_capabilities(jvmtiEnv *jvmti) {
    CapabilityBuffer caps;
    CapabilityBuffer have;

    memset(&caps, 0, sizeof(caps));
    caps.caps.can_retransform_classes = 1;
    add_capability(jvmti, &caps, "can_retransform_classes");

    memset(&caps, 0, sizeof(caps));
    caps.caps.can_retransform_any_class = 1;
    add_capability(jvmti, &caps, "can_retransform_any_class");

    memset(&caps, 0, sizeof(caps));
    caps.caps.can_generate_all_class_hook_events = 1;
    add_capability(jvmti, &caps, "can_generate_all_class_hook_events");

    memset(&have, 0, sizeof(have));
    if ((*jvmti)->GetCapabilities(jvmti, &have.caps) != JVMTI_ERROR_NONE) {
        return -1;
    }
    if (!have.caps.can_retransform_classes) {
        jlog("error: this JVM does not allow class retransformation");
        return -1;
    }
    jlog("capabilities: retransform=%d retransform-any=%d all-class-hook-events=%d",
         (int)have.caps.can_retransform_classes, (int)have.caps.can_retransform_any_class,
         (int)have.caps.can_generate_all_class_hook_events);
    return 0;
}

static void record_failure(jvmtiEnv *jvmti, jclass cls, jvmtiError err) {
    char *sig = NULL;
    char *generic = NULL;
    char name[MAX_CLASS_NAME] = "<unknown>";

    if ((*jvmti)->GetClassSignature(jvmti, cls, &sig, &generic) == JVMTI_ERROR_NONE && sig != NULL &&
        strlen(sig) > 2) {
        if (sig[0] == 'L' && sig[strlen(sig) - 1] == ';') {
            size_t len = strlen(sig) - 2;
            if (len < sizeof(name)) {
                memcpy(name, sig + 1, len);
                name[len] = '\0';
                for (size_t i = 0; name[i] != '\0'; i++) {
                    if (name[i] == '/') {
                        name[i] = '.';
                    }
                }
            }
        }
    }
    jlog("failed: %s (jvmti error %d)", name, (int)err);

    pthread_mutex_lock(&g_lock);
    jvmd_sv_push(&g.failed_names, name);
    g.failed++;
    pthread_mutex_unlock(&g_lock);

    if (sig != NULL) {
        (*jvmti)->Deallocate(jvmti, (unsigned char *)sig);
    }
    if (generic != NULL) {
        (*jvmti)->Deallocate(jvmti, (unsigned char *)generic);
    }
}

static int dump_classes(jvmtiEnv *jvmti, jclass *classes, size_t count) {
    jvmtiEventCallbacks callbacks;
    jvmtiError err;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.ClassFileLoadHook = &on_class_file_load_hook;

    err = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, sizeof(callbacks));
    if (err != JVMTI_ERROR_NONE) {
        jlog("error: SetEventCallbacks failed (jvmti error %d)", (int)err);
        return -1;
    }
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                             JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    if (err != JVMTI_ERROR_NONE) {
        jlog("error: cannot enable the class file load hook (jvmti error %d)", (int)err);
        return -1;
    }

    g.dumping = 1;
    for (size_t i = 0; i < count; i += BATCH_SIZE) {
        size_t n = (count - i < BATCH_SIZE) ? (count - i) : BATCH_SIZE;
        jclass batch[BATCH_SIZE];

        for (size_t k = 0; k < n; k++) {
            batch[k] = classes[i + k];
        }
        err = (*jvmti)->RetransformClasses(jvmti, (jint)n, batch);
        if (err != JVMTI_ERROR_NONE) {
            /* One bad class would fail the whole batch: retry one by one. */
            for (size_t k = 0; k < n; k++) {
                jvmtiError one = (*jvmti)->RetransformClasses(jvmti, 1, &batch[k]);
                if (one != JVMTI_ERROR_NONE) {
                    record_failure(jvmti, batch[k], one);
                }
            }
        }
    }
    g.dumping = 0;

    (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                       JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    return 0;
}

static int write_class_list(const JvmdStrVec *names) {
    char path[MAX_DIR_BUF];
    FILE *f;

    if (snprintf(path, sizeof(path), "%s/classes.txt", g.outdir) >= (int)sizeof(path)) {
        return -1;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    for (size_t i = 0; i < names->len; i++) {
        fprintf(f, "%s\n", names->items[i]);
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

static void write_failed_list(void) {
    char path[MAX_DIR_BUF];
    FILE *f;

    if (g.failed_names.len == 0) {
        return;
    }
    if (snprintf(path, sizeof(path), "%s/failed.txt", g.outdir) >= (int)sizeof(path)) {
        return;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    for (size_t i = 0; i < g.failed_names.len; i++) {
        fprintf(f, "%s\n", g.failed_names.items[i]);
    }
    fclose(f);
}

static void log_header(void) {
    char stamp[64];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(g.log, "%s %s\n", JVMD_NAME, JVMD_AGENT_VERSION);
    fprintf(g.log, "pid: %d\n", (int)getpid());
    fprintf(g.log, "started: %s\n", stamp);
    fprintf(g.log, "outdir: %s\n", g.outdir);
    fprintf(g.log, "mode: %s\n", g.list_only ? "list" : "dump");
    fflush(g.log);
}

static void log_summary(const char *status, double elapsed) {
    if (g.log == NULL) {
        return;
    }
    fprintf(g.log, "classes-loaded: %lu\n", g.loaded);
    fprintf(g.log, "classes-selected: %lu\n", g.selected);
    fprintf(g.log, "classes-filtered: %lu\n", g.filtered);
    fprintf(g.log, "files-written: %lu\n", g.written);
    fprintf(g.log, "bytes-written: %lu\n", g.bytes);
    fprintf(g.log, "duplicates: %lu\n", g.duplicates);
    fprintf(g.log, "unnamed: %lu\n", g.unnamed);
    fprintf(g.log, "failed-count: %lu\n", g.failed);
    fprintf(g.log, "elapsed: %.2fs\n", elapsed);
    fprintf(g.log, "status: %s\n", status);
    fflush(g.log);
}

static int prepare_output(void) {
    if (g.outdir[0] == '\0') {
        snprintf(g.outdir, sizeof(g.outdir), "/tmp/jvmdump-%d", (int)getpid());
    }
    if (jvmd_mkdir_p(g.outdir, 0755) != 0) {
        fprintf(stderr, "%s: cannot create '%s': %s\n", JVMD_NAME, g.outdir, strerror(errno));
        return -1;
    }
    if (snprintf(g.logpath, sizeof(g.logpath), "%s/dump.log", g.outdir) >= (int)sizeof(g.logpath)) {
        return -1;
    }
    g.log = fopen(g.logpath, "w");
    if (g.log == NULL) {
        fprintf(stderr, "%s: cannot write '%s': %s\n", JVMD_NAME, g.logpath, strerror(errno));
        return -1;
    }
    return 0;
}

static jint run_dump(JavaVM *vm, const char *options) {
    jvmtiEnv *jvmti = NULL;
    jclass *classes = NULL;
    jclass *selected_list = NULL;
    jint class_count = 0;
    jvmtiError err;
    double started;
    const char *status = "ok";
    size_t selected = 0;
    JvmdStrVec names = { 0 };

    /* Reset the shared state; the mutex itself is static and never destroyed. */
    memset(&g, 0, sizeof(g));

    if (parse_options(options) != 0) {
        fprintf(stderr, "%s: invalid agent options: %s\n", JVMD_NAME, options != NULL ? options : "");
        return JVMTI_ERROR_ILLEGAL_ARGUMENT;
    }
    if (prepare_output() != 0) {
        return JVMTI_ERROR_INTERNAL;
    }
    started = jvmd_now();
    log_header();

    if (get_jvmti_env(vm, &jvmti) != JNI_OK) {
        jlog("error: JVMTI is not available in this JVM");
        log_summary("error: no JVMTI support", jvmd_now() - started);
        return JVMTI_ERROR_INTERNAL;
    }
    g.jvmti = jvmti;

    if (!g.list_only && enable_capabilities(jvmti) != 0) {
        log_summary("error: missing capabilities", jvmd_now() - started);
        return JVMTI_ERROR_MUST_POSSESS_CAPABILITY;
    }

    err = (*jvmti)->GetLoadedClasses(jvmti, &class_count, &classes);
    if (err != JVMTI_ERROR_NONE) {
        jlog("error: GetLoadedClasses failed (jvmti error %d)", (int)err);
        log_summary("error: cannot enumerate classes", jvmd_now() - started);
        return JVMTI_ERROR_INTERNAL;
    }
    g.loaded = (unsigned long)class_count;

    if (!g.list_only) {
        selected_list = calloc((size_t)class_count + 1, sizeof(*selected_list));
        if (selected_list == NULL) {
            (*jvmti)->Deallocate(jvmti, (unsigned char *)classes);
            log_summary("error: out of memory", jvmd_now() - started);
            return JVMTI_ERROR_OUT_OF_MEMORY;
        }
    }

    for (jint i = 0; i < class_count; i++) {
        char *sig = NULL;
        char *generic = NULL;

        if ((*jvmti)->GetClassSignature(jvmti, classes[i], &sig, &generic) != JVMTI_ERROR_NONE ||
            sig == NULL) {
            if (sig != NULL) {
                (*jvmti)->Deallocate(jvmti, (unsigned char *)sig);
            }
            if (generic != NULL) {
                (*jvmti)->Deallocate(jvmti, (unsigned char *)generic);
            }
            continue;
        }

        /* Array and primitive classes have no class file of their own. */
        if (strlen(sig) > 2 && sig[0] == 'L' && sig[strlen(sig) - 1] == ';') {
            size_t len = strlen(sig) - 2;
            char dotted[MAX_CLASS_NAME];

            if (len > 0 && len < sizeof(dotted)) {
                memcpy(dotted, sig + 1, len);
                dotted[len] = '\0';
                if (internal_to_dotted(dotted, dotted, sizeof(dotted)) == 0) {
                    if (class_selected(dotted)) {
                        g.selected++;
                        if (g.list_only) {
                            if (jvmd_sv_push(&names, dotted) != 0) {
                                jlog("warning: out of memory while collecting class names");
                            }
                        } else {
                            selected_list[selected++] = classes[i];
                        }
                    } else {
                        g.filtered++;
                    }
                }
            }
        } else {
            g.filtered++;
        }

        (*jvmti)->Deallocate(jvmti, (unsigned char *)sig);
        if (generic != NULL) {
            (*jvmti)->Deallocate(jvmti, (unsigned char *)generic);
        }
    }

    if (g.list_only) {
        jvmd_sv_sort(&names);
        if (write_class_list(&names) != 0) {
            jlog("error: cannot write the class list: %s", strerror(errno));
            status = "error: cannot write class list";
        }
        g.written = (unsigned long)names.len;
        jlog("listed %lu classes", g.selected);
    } else {
        char manifest_path[MAX_DIR_BUF];

        if (snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.tsv", g.outdir) >=
            (int)sizeof(manifest_path)) {
            status = "error: path too long";
        } else {
            g.manifest = fopen(manifest_path, "w");
        }

        if (selected > 0) {
            if (dump_classes(jvmti, selected_list, selected) != 0) {
                status = "error: cannot dump classes";
            }
        }

        if (g.manifest != NULL) {
            fclose(g.manifest);
            g.manifest = NULL;
        }
        jlog("dumped %lu class files (%lu bytes)", g.written, g.bytes);
    }

    write_failed_list();
    log_summary(status, jvmd_now() - started);

    if (classes != NULL) {
        (*jvmti)->Deallocate(jvmti, (unsigned char *)classes);
    }
    free(selected_list);
    jvmd_sv_free(&names);
    jvmd_sv_free(&g.failed_names);
    name_table_free();

    if (g.log != NULL) {
        fclose(g.log);
        g.log = NULL;
    }
    if (strcmp(status, "ok") == 0) {
        return JVMTI_ERROR_NONE;
    }
    return JVMTI_ERROR_INTERNAL;
}

static void JNICALL on_vm_init(jvmtiEnv *jvmti, JNIEnv *env, jthread thread) {
    (void)jvmti;
    (void)env;
    (void)thread;
    if (g_startup_options == NULL) {
        return; /* a delayed dump thread already owns the options */
    }
    run_dump(g.vm, g_startup_options);
    free(g_startup_options);
    g_startup_options = NULL;
}

/* Optional "delay=N" for -agentpath: dumps N seconds after VMInit instead of
 * immediately, so that the application has had a chance to load its classes. */
static int parse_delay(const char *options) {
    const char *p;

    if (options == NULL) {
        return 0;
    }
    p = strstr(options, "delay=");
    if (p == NULL || (p != options && p[-1] != ',')) {
        return 0;
    }
    int value = atoi(p + 6);
    return value > 0 && value <= 86400 ? value : 0;
}

static void *delayed_dump(void *arg) {
    char *options = arg;
    JavaVM *vm = g.vm;
    JNIEnv *env = NULL;
    int seconds = g.delay_seconds;

    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
    if ((*vm)->AttachCurrentThreadAsDaemon(vm, (void **)&env, NULL) == JNI_OK) {
        run_dump(vm, options);
        (*vm)->DetachCurrentThread(vm);
    } else {
        fprintf(stderr, "%s: cannot attach the dump thread to the JVM\n", JVMD_NAME);
    }
    free(options);
    return NULL;
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;
    /* Undo the short stderr redirection that hides HotSpot's dynamic agent
     * warning.  Must stay the first statement. */
    restore_stderr();
    /* Always return JNI_OK once we ran: the JVM unloads the library otherwise,
     * and the caller learns the details from <outdir>/dump.log. */
    run_dump(vm, options);
    return JNI_OK;
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    jvmtiEnv *jvmti = NULL;
    jvmtiEventCallbacks callbacks;
    jvmtiError err;

    (void)reserved;
    /* Undo the short stderr redirection; must stay the first statement. */
    restore_stderr();
    if (get_jvmti_env(vm, &jvmti) != JNI_OK) {
        return JNI_ERR;
    }
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.VMInit = &on_vm_init;
    err = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, sizeof(callbacks));
    if (err != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    if (err != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }
    g.vm = vm;
    g.delay_seconds = parse_delay(options);
    g_startup_options = (options != NULL) ? strdup(options) : NULL;
    if (g.delay_seconds > 0) {
        pthread_t thread;
        char *opts = g_startup_options;

        g_startup_options = NULL;
        if (pthread_create(&thread, NULL, delayed_dump, opts) == 0) {
            pthread_detach(thread);
            return JNI_OK;
        }
        free(opts);
        g.delay_seconds = 0;
    }
    return JNI_OK;
}
