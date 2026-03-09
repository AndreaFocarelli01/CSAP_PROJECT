#define _POSIX_C_SOURCE 200809L

/*
 * history.c — Ring buffer per la history dei comandi
 *
 * Funzionalità:
 *   history      — mostra lista numerata
 *   !!           — ripete l'ultimo comando
 *   !N           — ripete il comando N (indice assoluto, 1-based)
 */

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HISTORY    64
#define HISTORY_FILE   ".vhds_history"   /* relativo a $HOME */

static char g_history[MAX_HISTORY][INPUT_BUFSIZE];
static int  g_hist_count = 0;   /* totale comandi inseriti (mai decrementato) */
static char g_hist_path[MAX_PATH];  /* path assoluto del file di history */

/* ─────────────────────────────────────────────
 * history_init: carica history da file $HOME/.vhds_history
 * Da chiamare una volta all'avvio.
 * ───────────────────────────────────────────── */
void history_init(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    snprintf(g_hist_path, sizeof(g_hist_path),
             "%s/%s", home, HISTORY_FILE);

    FILE *f = fopen(g_hist_path, "r");
    if (!f) return;

    char line[INPUT_BUFSIZE];
    while (fgets(line, sizeof(line), f)) {
        /* Rimuovi newline finale */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (line[0] == '\0') continue;
        int slot = g_hist_count % MAX_HISTORY;
        safe_strncpy(g_history[slot], line, INPUT_BUFSIZE);
        g_hist_count++;
    }
    fclose(f);
}

/* ─────────────────────────────────────────────
 * history_save: scrive le ultime MAX_HISTORY voci su file
 * Da chiamare all'uscita del client.
 * ───────────────────────────────────────────── */
void history_save(void) {
    if (g_hist_path[0] == '\0') return;

    FILE *f = fopen(g_hist_path, "w");
    if (!f) return;

    int start = (g_hist_count > MAX_HISTORY)
                 ? g_hist_count - MAX_HISTORY : 0;
    for (int i = start; i < g_hist_count; i++) {
        fprintf(f, "%s\n", g_history[i % MAX_HISTORY]);
    }
    fclose(f);
}

/* ─────────────────────────────────────────────
 * history_add
 * ───────────────────────────────────────────── */
void history_add(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    if (strcmp(cmd, "history") == 0) return;   /* non registrare "history" */

    int slot = g_hist_count % MAX_HISTORY;
    safe_strncpy(g_history[slot], cmd, INPUT_BUFSIZE);
    g_hist_count++;
}

/* ─────────────────────────────────────────────
 * history_print
 * ───────────────────────────────────────────── */
void history_print(void) {
    int start = (g_hist_count > MAX_HISTORY)
                 ? g_hist_count - MAX_HISTORY : 0;

    if (start == g_hist_count) {
        printf("  (no history)\n");
        return;
    }

    for (int i = start; i < g_hist_count; i++) {
        int slot = i % MAX_HISTORY;
        printf("  %3d  %s\n", i + 1, g_history[slot]);
    }
    fflush(stdout);
}

/* ─────────────────────────────────────────────
 * Accesso interno: per indice assoluto (1-based)
 * ───────────────────────────────────────────── */
static const char *history_get_abs(int n) {
    if (n < 1 || n > g_hist_count) return NULL;
    int oldest = (g_hist_count > MAX_HISTORY)
                  ? g_hist_count - MAX_HISTORY + 1 : 1;
    if (n < oldest) return NULL;
    return g_history[(n - 1) % MAX_HISTORY];
}

/* ─────────────────────────────────────────────
 * history_expand
 *
 * Se cmd inizia per '!', espande:
 *   !!  → ultimo comando
 *   !N  → comando N (assoluto)
 *
 * Ritorna puntatore a buffer statico se espansione riuscita,
 *         "" se magic non trovato (errore già stampato),
 *         NULL se cmd non inizia con '!' (nessuna espansione).
 * ───────────────────────────────────────────── */
const char *history_expand(const char *cmd) {
    static char expanded[INPUT_BUFSIZE];

    if (cmd[0] != '!') return NULL;

    /* !! → ultimo */
    if (cmd[1] == '!' && cmd[2] == '\0') {
        if (g_hist_count == 0) {
            fprintf(stderr, "No previous command.\n");
            return "";
        }
        const char *last = history_get_abs(g_hist_count);
        printf("  %s\n", last);
        safe_strncpy(expanded, last, sizeof(expanded));
        return expanded;
    }

    /* !N → indice assoluto */
    char *endp;
    long  n = strtol(cmd + 1, &endp, 10);
    if (*endp == '\0' && n > 0) {
        const char *entry = history_get_abs((int)n);
        if (!entry) {
            fprintf(stderr, "No such history entry: %ld\n", n);
            return "";
        }
        printf("  %s\n", entry);
        safe_strncpy(expanded, entry, sizeof(expanded));
        return expanded;
    }

    return NULL;   /* non è un magic */
}
