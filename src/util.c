/*
 * jvmdump - shared helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

int jvmd_mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    size_t len;

    if (path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }

    struct stat st;
    if (stat(tmp, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int jvmd_glob_match(const char *pattern, const char *text) {
    while (*pattern != '\0') {
        switch (*pattern) {
        case '*':
            while (*pattern == '*') {
                pattern++;
            }
            if (*pattern == '\0') {
                return 1;
            }
            for (const char *s = text;; s++) {
                if (jvmd_glob_match(pattern, s)) {
                    return 1;
                }
                if (*s == '\0') {
                    return 0;
                }
            }
        case '?':
            if (*text == '\0') {
                return 0;
            }
            pattern++;
            text++;
            break;
        default:
            if (*pattern != *text) {
                return 0;
            }
            pattern++;
            text++;
            break;
        }
    }
    return *text == '\0';
}

int jvmd_read_uids(pid_t pid, uid_t *uid, gid_t *gid) {
    char path[64];
    char line[512];
    FILE *f;
    int found = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned first, second;
        if (strncmp(line, "Uid:", 4) == 0 && sscanf(line + 4, "%u %u", &first, &second) == 2) {
            *uid = (uid_t)second; /* effective uid */
            found = 1;
        } else if (strncmp(line, "Gid:", 4) == 0 && sscanf(line + 4, "%u %u", &first, &second) == 2) {
            *gid = (gid_t)second; /* effective gid */
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int jvmd_process_has_libjvm(pid_t pid) {
    char path[64];
    char line[1024];
    FILE *f;
    int found = 0;

    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "libjvm.so") != NULL) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

double jvmd_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int jvmd_sv_push(JvmdStrVec *v, const char *s) {
    char *copy;

    if (v->len == v->cap) {
        size_t ncap = v->cap == 0 ? 16 : v->cap * 2;
        char **items = realloc(v->items, ncap * sizeof(*items));
        if (items == NULL) {
            return -1;
        }
        v->items = items;
        v->cap = ncap;
    }
    copy = strdup(s);
    if (copy == NULL) {
        return -1;
    }
    v->items[v->len++] = copy;
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void jvmd_sv_sort(JvmdStrVec *v) {
    if (v->len > 1) {
        qsort(v->items, v->len, sizeof(*v->items), cmp_str);
    }
}

void jvmd_sv_free(JvmdStrVec *v) {
    for (size_t i = 0; i < v->len; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}
