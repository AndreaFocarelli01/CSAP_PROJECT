#ifndef SESSION_H
#define SESSION_H

#include <sys/types.h>
#include "protocol.h"

#define MAX_USERS       128
#define MAX_SESSIONS    256
#define SERVER_GROUP    "vhds_users"   /* gruppo comune a tutti gli utenti */

/* ─────────────────────────────────────────────
 * Record utente (in Shared Memory)
 * ───────────────────────────────────────────── */
typedef struct {
    int    valid;                   /* 1 = slot occupato */
    char   username[MAX_USERNAME];
    char   home[MAX_PATH];          /* path assoluto home nel VFS */
    mode_t home_perm;               /* permessi ottali della home */
    uid_t  uid;                     /* uid di sistema assegnato   */
    gid_t  gid;                     /* gid del gruppo comune      */
} UserRecord;

/* ─────────────────────────────────────────────
 * Sessione attiva per worker (in Shared Memory)
 * ───────────────────────────────────────────── */
typedef struct {
    int    valid;                   /* 1 = sessione attiva        */
    char   username[MAX_USERNAME];
    char   cwd[MAX_PATH];           /* directory di lavoro corrente */
    pid_t  worker_pid;              /* pid del processo worker    */
    int    socket_fd;               /* fd del socket (solo nel worker stesso) */
    int    bg_count;                /* job background attivi      */
} Session;

/* ─────────────────────────────────────────────
 * Tabella condivisa (allocata in SHM)
 * ───────────────────────────────────────────── */
typedef struct {
    UserRecord users[MAX_USERS];
    Session    sessions[MAX_SESSIONS];
    int        users_sem;           /* sem System V: mutex tabella utenti  */
    int        sessions_sem;        /* sem System V: mutex tabella sessioni */
} SessionTable;

/* ─────────────────────────────────────────────
 * API
 * ───────────────────────────────────────────── */

/*
 * Alloca/collega la SessionTable in shared memory.
 * `create` = 1 → crea (lato server all'avvio), 0 → collega (lato worker).
 * Ritorna puntatore alla tabella, NULL su errore.
 */
SessionTable *session_table_init(int create);

/* Distacca la SHM dal processo corrente. */
void          session_table_detach(SessionTable *tbl);

/* Distrugge la SHM (solo il padre al termine). */
void          session_table_destroy(void);

/*
 * Registra un nuovo utente. Ritorna 0 su successo, -1 se già esiste o piena.
 */
int  session_add_user(SessionTable *tbl, const char *username,
                      const char *home, mode_t perm, uid_t uid, gid_t gid);

/* Cerca un utente per nome. Ritorna puntatore (read-only) o NULL. */
const UserRecord *session_find_user(SessionTable *tbl, const char *username);

/*
 * Apre una sessione per `username` nel worker `pid`.
 * Popola `sess_idx` con l'indice allocato.
 * Ritorna 0 su successo, -1 su errore.
 */
int  session_open(SessionTable *tbl, const char *username,
                  pid_t pid, int *sess_idx);

/* Chiude la sessione all'indice `sess_idx`. */
void session_close(SessionTable *tbl, int sess_idx);

/*
 * Cerca la sessione attiva di `username`.
 * Ritorna il pid del worker, 0 se offline.
 */
pid_t session_get_pid(SessionTable *tbl, const char *username);

/*
 * Aggiorna la cwd della sessione `sess_idx`.
 */
void session_set_cwd(SessionTable *tbl, int sess_idx, const char *cwd);

/*
 * Incrementa/decrementa contatore job background.
 */
void session_bg_inc(SessionTable *tbl, int sess_idx);
void session_bg_dec(SessionTable *tbl, int sess_idx);
int  session_bg_count(SessionTable *tbl, int sess_idx);

/* Debug */
void session_dump_users(const SessionTable *tbl);

/* Purga sessioni orfane (worker morto senza session_close) */
void session_cleanup_dead(SessionTable *tbl);

#endif /* SESSION_H */
