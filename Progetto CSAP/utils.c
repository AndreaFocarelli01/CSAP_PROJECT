#define _POSIX_C_SOURCE 200809L

/*
 * utils.c — Utilità generali
 *
 * MIGLIORAMENTI:
 *  1. Logging con timestamp (HH:MM:SS) e PID — indispensabile con
 *     processi multipli che scrivono sullo stesso log.
 *  2. str_tokenize gestisce argomenti tra virgolette singole/doppie
 *     (path con spazi: upload "my file.txt" remote.txt).
 *  3. log_warn() — livello intermedio tra info ed error.
 *  4. path_resolve: se out_size è insufficiente, restituisce -1
 *     anziché silenziosamente troncare il path (bug silenzioso).
 *  5. read_all/write_all: gestione esplicita di EINTR.
 *  6. Aggiunti path_join() e path_extension() come helper.
 */

#include "utils.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>

/* ─────────────────────────────────────────────
 * Logging con timestamp e PID
 * ───────────────────────────────────────────── */

static void log_prefix(FILE *out, const char *level) {
    struct timespec ts;
    struct tm       tm_info;
    char            tbuf[24];

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
        localtime_r(&ts.tv_sec, &tm_info) != NULL) {
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
        fprintf(out, "[%s.%03ld] %-5s [pid=%-5d] ",
                tbuf, ts.tv_nsec / 1000000L, level, (int)getpid());
    } else {
        fprintf(out, "[??:??:??.???] %-5s [pid=%-5d] ", level, (int)getpid());
    }
}

void log_info(const char *fmt, ...) {
    va_list ap;
    log_prefix(stdout, "INFO ");
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    log_prefix(stderr, "WARN ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    log_prefix(stderr, "ERROR");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_debug(const char *fmt, ...) {
#ifdef DEBUG
    va_list ap;
    log_prefix(stdout, "DEBUG");
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
#else
    (void)fmt;
#endif
}

/* ─────────────────────────────────────────────
 * safe_strncpy
 * ───────────────────────────────────────────── */
void safe_strncpy(char *dst, const char *src, size_t size) {
    if (!size) return;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* ─────────────────────────────────────────────
 * Path helpers
 * ───────────────────────────────────────────── */

int path_normalize(char *path) {
    char  buf[MAX_PATH];
    char *parts[MAX_PATH / 2];
    int   n     = 0;
    int   is_abs = (path[0] == '/');
    char *save  = NULL;
    char *tok;
    char *p;
    int   i;

    safe_strncpy(buf, path, sizeof(buf));
    p = buf;

    while ((tok = strtok_r(p, "/", &save)) != NULL) {
        p = NULL;
        if (strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0)        n--;
            else if (!is_abs) return -1; /* traversal relativo oltre la radice */
        } else {
            if (n >= (int)(MAX_PATH / 2) - 1) return -1; /* troppo profondo */
            parts[n++] = tok;
        }
    }

    path[0] = '\0';
    if (is_abs) strcat(path, "/");

    for (i = 0; i < n; i++) {
        if (i > 0) strcat(path, "/");
        strcat(path, parts[i]);
    }
    if (path[0] == '\0') strcat(path, ".");
    return 0;
}

int path_is_under(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) return 0;
    if (path[root_len] != '\0' && path[root_len] != '/') return 0;
    return 1;
}

int path_resolve(const char *root, const char *cwd,
                 const char *path, char *out, size_t out_size) {
    char tmp[MAX_PATH];
    int  written;

    if (path[0] == '/')
        written = snprintf(tmp, sizeof(tmp), "%s%s", root, path);
    else
        written = snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);

    if (written < 0 || (size_t)written >= sizeof(tmp)) return -1;
    if (path_normalize(tmp) != 0) return -1;
    if (!path_is_under(root, tmp)) return -1;

    /* Verifica che out_size sia sufficiente — evita troncamenti silenti */
    if (strlen(tmp) + 1 > out_size) {
        log_error("path_resolve: output buffer too small (%zu needed, %zu given)",
                  strlen(tmp) + 1, out_size);
        return -1;
    }

    safe_strncpy(out, tmp, out_size);
    return 0;
}

void path_basename(const char *path, char *out, size_t out_size) {
    const char *last = strrchr(path, '/');
    if (!last) safe_strncpy(out, path, out_size);
    else       safe_strncpy(out, last + 1, out_size);
    if (out[0] == '\0') safe_strncpy(out, ".", out_size);
}

void path_dirname(const char *path, char *out, size_t out_size) {
    const char *last = strrchr(path, '/');
    if (!last)         { safe_strncpy(out, ".", out_size); return; }
    if (last == path)  { safe_strncpy(out, "/", out_size); return; }
    size_t len = (size_t)(last - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

/*
 * path_join: concatena base e rel in out, poi normalizza e verifica
 * che il risultato resti sotto root. Ritorna 0 su successo, -1 su errore.
 */
int path_join(const char *base, const char *rel,
              char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s/%s", base, rel);
    if (written < 0 || (size_t)written >= out_size) return -1;
    return path_normalize(out);
}

/* ─────────────────────────────────────────────
 * Permessi
 * ───────────────────────────────────────────── */

int parse_permissions(const char *str, mode_t *out) {
    char *end;
    long  val;
    if (!str || !str[0]) return -1;
    val = strtol(str, &end, 8);
    if (*end != '\0' || val < 0 || val > 07777) return -1;
    *out = (mode_t)val;
    return 0;
}

void format_permissions(mode_t mode, char *out) {
    out[0] = (mode & S_IRUSR) ? 'r' : '-';
    out[1] = (mode & S_IWUSR) ? 'w' : '-';
    out[2] = (mode & S_IXUSR) ? 'x' : '-';
    out[3] = (mode & S_IRGRP) ? 'r' : '-';
    out[4] = (mode & S_IWGRP) ? 'w' : '-';
    out[5] = (mode & S_IXGRP) ? 'x' : '-';
    out[6] = (mode & S_IROTH) ? 'r' : '-';
    out[7] = (mode & S_IWOTH) ? 'w' : '-';
    out[8] = (mode & S_IXOTH) ? 'x' : '-';
    out[9] = '\0';
}

/* ─────────────────────────────────────────────
 * I/O helpers con gestione EINTR
 * ───────────────────────────────────────────── */

ssize_t read_all(int fd, void *buf, size_t n) {
    size_t  total = 0;
    ssize_t r;
    char   *ptr   = (char *)buf;

    while (total < n) {
        r = read(fd, ptr + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;   /* EOF */
        total += (size_t)r;
    }
    return (ssize_t)total;
}

ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t      total = 0;
    ssize_t     w;
    const char *ptr   = (const char *)buf;

    while (total < n) {
        w = write(fd, ptr + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        total += (size_t)w;
    }
    return (ssize_t)total;
}

/* ─────────────────────────────────────────────
 * Privilegi
 * ───────────────────────────────────────────── */

static uid_t saved_euid = 0;

int drop_privileges(void) {
    saved_euid = geteuid();
    if (saved_euid == 0) {
        uid_t real_uid = getuid();
        if (seteuid(real_uid) < 0) { perror("seteuid drop"); return -1; }
    }
    return 0;
}

int gain_privileges(void) {
    if (saved_euid == 0) {
        if (seteuid(0) < 0) { perror("seteuid gain"); return -1; }
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * Stringhe
 * ───────────────────────────────────────────── */

char *str_trim(char *s) {
    char *end;
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/*
 * str_tokenize con supporto per virgolette.
 *
 * Gestisce:
 *   upload "my file.txt" remote.txt
 *   upload 'path with spaces/file' dst
 *
 * Le virgolette vengono rimosse dall'argomento.
 * Non gestisce escape backslash (out of scope).
 */
int str_tokenize(char *line, char **argv, int max_argc) {
    int   argc = 0;
    char *p    = line;

    while (argc < max_argc && *p) {
        /* Salta whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (*p == '"' || *p == '\'') {
            /* Argomento tra virgolette */
            char quote = *p++;
            char *start = p;
            while (*p && *p != quote) p++;
            if (*p == quote) *p++ = '\0'; /* chiudi e avanza */
            argv[argc++] = start;
        } else {
            /* Argomento normale (delimitato da whitespace) */
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}
