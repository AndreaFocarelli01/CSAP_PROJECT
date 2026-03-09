#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <sys/types.h>

/* ─────────────────────────────────────────────
 * Logging
 * ───────────────────────────────────────────── */
void log_info (const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_debug(const char *fmt, ...);

/* ─────────────────────────────────────────────
 * Path helpers
 * ───────────────────────────────────────────── */

/*
 * Risolve `path` relativo a `cwd` e lo normalizza (elimina . e ..).
 * Verifica che il risultato sia all'interno di `root`.
 * Scrive il path assoluto in `out` (size = out_size).
 * Ritorna 0 su successo, -1 se il path tenta di uscire da root.
 */
int  path_resolve(const char *root, const char *cwd,
                  const char *path, char *out, size_t out_size);

/*
 * Normalizza un path eliminando sequenze "/./", "/../", doppi slash.
 * Modifica `path` in-place. Ritorna 0 su successo, -1 se va sopra "/".
 */
int  path_normalize(char *path);

/*
 * Controlla che `path` sia contenuto in `root` (entrambi normalizzati).
 * Ritorna 1 se ok, 0 se esce da root.
 */
int  path_is_under(const char *root, const char *path);

/*
 * Estrae il nome base da un path (basename non rientrante).
 * Scrive in `out` (size = out_size).
 */
void path_basename(const char *path, char *out, size_t out_size);

/*
 * Estrae la directory da un path (dirname non rientrante).
 * Scrive in `out` (size = out_size).
 */
void path_dirname(const char *path, char *out, size_t out_size);
int  path_join(const char *base, const char *rel, char *out, size_t out_size);

/* ─────────────────────────────────────────────
 * Parsing permessi ottali
 * ───────────────────────────────────────────── */

/* Converte stringa ottale ("0755") in mode_t. Ritorna -1 se invalida. */
int  parse_permissions(const char *str, mode_t *out);

/* Formatta mode_t come stringa "rwxrwxrwx". out deve essere >= 10 byte. */
void format_permissions(mode_t mode, char *out);

/* ─────────────────────────────────────────────
 * I/O helpers
 * ───────────────────────────────────────────── */

/* read() garantito: rilegge finché non ha letto esattamente n byte o EOF/errore */
ssize_t read_all (int fd, void *buf, size_t n);

/* write() garantito: riscrive finché non ha scritto esattamente n byte o errore */
ssize_t write_all(int fd, const void *buf, size_t n);

/* ─────────────────────────────────────────────
 * Privilegi
 * ───────────────────────────────────────────── */

/* Salva euid root e scende a uid reale. Ritorna 0 se ok. */
int  drop_privileges(void);

/* Riacquisisce temporaneamente root. Ritorna 0 se ok. */
int  gain_privileges(void);

/* ─────────────────────────────────────────────
 * Miscellanea
 * ───────────────────────────────────────────── */

/* Copia sicura stringa (sempre null-termina) */
void safe_strncpy(char *dst, const char *src, size_t size);

/* Trim whitespace in-place (ritorna ptr al primo char non-ws) */
char *str_trim(char *s);

/* Divide `line` in token (simile a strtok_r). Riempie argv, ritorna argc. */
int  str_tokenize(char *line, char **argv, int max_argc);

#endif /* UTILS_H */
