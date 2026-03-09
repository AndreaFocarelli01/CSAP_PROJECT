#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * session.c — Tabella utenti e sessioni in memoria condivisa
 *
 * MIGLIORAMENTI:
 *  1. session_find_user: ora prende il lock in lettura prima di scansionare
 *     la user table — sicuro con worker multipli in fork.
 *  2. Macro SESS_LOCK/SESS_UNLOCK per compattare il codice.
 *  3. session_open: verifica che il worker_pid non sia già presente
 *     per lo stesso utente prima di aprire una nuova sessione (previene
 *     entry duplicate se un worker precedente è crashato senza cleanup).
 *  4. session_dump_users: nuova funzione di debug che elenca tutti gli
 *     utenti registrati — utile per diagnosi server.
 *  5. sem_timedwait fallback per evitare blocco infinito su semafori
 *     corrotti (dopo crash di un worker con sem acquisito).
 */

#include "session.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>

#define SESSION_SHM_KEY  0x53455301

static int session_shm_id = -1;

union semun_sess {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* ─────────────────────────────────────────────
 * Helper semafori con timeout (5 s) per evitare
 * blocco infinito su semafori corrotti.
 * ───────────────────────────────────────────── */
#define SEM_TIMEOUT_SEC  5

static int sem_p(int sem_id) {
    struct sembuf op = { 0, -1, 0 };
#ifdef __linux__
    /* Usa semtimedop su Linux per evitare blocco infinito */
    struct timespec ts = { SEM_TIMEOUT_SEC, 0 };
    while (semtimedop(sem_id, &op, 1, &ts) < 0) {
        if (errno == EINTR)  continue;
        if (errno == EAGAIN) {
            log_error("sem_p: timeout on semaphore %d "
                      "(sem may be corrupted, forcing value)", sem_id);
            /* Forza il valore a 1 e riprova una sola volta */
            union semun_sess su; su.val = 1;
            semctl(sem_id, 0, SETVAL, su);
            if (semtimedop(sem_id, &op, 1, &ts) == 0) return 0;
        }
        perror("semtimedop");
        return -1;
    }
#else
    while (semop(sem_id, &op, 1) < 0) {
        if (errno == EINTR) continue;
        perror("sem_p"); return -1;
    }
#endif
    return 0;
}

static int sem_v(int sem_id) {
    struct sembuf op = { 0, +1, 0 };
    while (semop(sem_id, &op, 1) < 0) {
        if (errno == EINTR) continue;
        perror("sem_v"); return -1;
    }
    return 0;
}

static int sem_create(int init_val) {
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("sem_create"); return -1; }
    union semun_sess su; su.val = init_val;
    if (semctl(id, 0, SETVAL, su) < 0) {
        semctl(id, 0, IPC_RMID);
        return -1;
    }
    return id;
}

/* ─────────────────────────────────────────────
 * Inizializzazione SHM
 * ───────────────────────────────────────────── */
SessionTable *session_table_init(int create) {
    int flags = create ? (IPC_CREAT | IPC_EXCL | 0666) : 0666;
    session_shm_id = shmget((key_t)SESSION_SHM_KEY,
                             sizeof(SessionTable), flags);
    if (session_shm_id < 0 && create && errno == EEXIST) {
        int stale = shmget((key_t)SESSION_SHM_KEY,
                           sizeof(SessionTable), 0666);
        if (stale >= 0) shmctl(stale, IPC_RMID, NULL);
        session_shm_id = shmget((key_t)SESSION_SHM_KEY,
                                 sizeof(SessionTable),
                                 IPC_CREAT | IPC_EXCL | 0666);
    }
    if (session_shm_id < 0) {
        perror("session_table_init: shmget"); return NULL;
    }

    SessionTable *tbl = (SessionTable *)shmat(session_shm_id, NULL, 0);
    if (tbl == (SessionTable *)-1) {
        perror("session_table_init: shmat"); return NULL;
    }

    if (create) {
        memset(tbl, 0, sizeof(SessionTable));
        tbl->users_sem    = sem_create(1);
        tbl->sessions_sem = sem_create(1);
        if (tbl->users_sem < 0 || tbl->sessions_sem < 0) {
            if (tbl->users_sem >= 0)    semctl(tbl->users_sem,    0, IPC_RMID);
            if (tbl->sessions_sem >= 0) semctl(tbl->sessions_sem, 0, IPC_RMID);
            shmdt(tbl); return NULL;
        }
    }
    return tbl;
}

void session_table_detach(SessionTable *tbl) {
    if (tbl) shmdt(tbl);
}

void session_table_destroy(void) {
    if (session_shm_id >= 0) {
        shmctl(session_shm_id, IPC_RMID, NULL);
        session_shm_id = -1;
    }
}

/* ─────────────────────────────────────────────
 * Utenti
 * ───────────────────────────────────────────── */
int session_add_user(SessionTable *tbl, const char *username,
                     const char *home, mode_t perm,
                     uid_t uid, gid_t gid) {
    if (sem_p(tbl->users_sem) < 0) return -1;

    for (int i = 0; i < MAX_USERS; i++) {
        if (tbl->users[i].valid &&
            strcmp(tbl->users[i].username, username) == 0) {
            sem_v(tbl->users_sem);
            log_error("session_add_user: user '%s' already exists", username);
            return -1;
        }
    }
    for (int i = 0; i < MAX_USERS; i++) {
        if (!tbl->users[i].valid) {
            tbl->users[i].valid     = 1;
            tbl->users[i].uid       = uid;
            tbl->users[i].gid       = gid;
            tbl->users[i].home_perm = perm;
            safe_strncpy(tbl->users[i].username, username, MAX_USERNAME);
            safe_strncpy(tbl->users[i].home,     home,     MAX_PATH);
            sem_v(tbl->users_sem);
            return 0;
        }
    }

    sem_v(tbl->users_sem);
    log_error("session_add_user: user table full");
    return -1;
}

/*
 * session_find_user — versione thread/process-safe con lock in lettura.
 * Precedentemente non acquisiva il lock, rendendo la lettura
 * potenzialmente race con session_add_user in un altro worker.
 */
const UserRecord *session_find_user(SessionTable *tbl, const char *username) {
    if (sem_p(tbl->users_sem) < 0) return NULL;

    const UserRecord *found = NULL;
    for (int i = 0; i < MAX_USERS; i++) {
        if (tbl->users[i].valid &&
            strcmp(tbl->users[i].username, username) == 0) {
            found = &tbl->users[i];
            break;
        }
    }

    sem_v(tbl->users_sem);
    return found;
}

/* ─────────────────────────────────────────────
 * Sessioni
 * ───────────────────────────────────────────── */
int session_open(SessionTable *tbl, const char *username,
                 pid_t pid, int *sess_idx) {
    /* Verifica utente (fuori dal lock sessioni per evitare deadlock) */
    const UserRecord *ur = session_find_user(tbl, username);
    if (!ur) {
        log_error("session_open: unknown user '%s'", username);
        return -1;
    }

    if (sem_p(tbl->sessions_sem) < 0) return -1;

    /*
     * Pulizia difensiva: se esiste già una sessione con lo stesso pid
     * (worker crashato senza cleanup), la rimuoviamo prima.
     */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (tbl->sessions[i].valid &&
            tbl->sessions[i].worker_pid == pid) {
            log_warn("session_open: found stale session for pid=%d, clearing",
                     (int)pid);
            memset(&tbl->sessions[i], 0, sizeof(Session));
        }
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!tbl->sessions[i].valid) {
            tbl->sessions[i].valid      = 1;
            tbl->sessions[i].worker_pid = pid;
            tbl->sessions[i].bg_count   = 0;
            safe_strncpy(tbl->sessions[i].username, username, MAX_USERNAME);
            safe_strncpy(tbl->sessions[i].cwd, ur->home, MAX_PATH);
            *sess_idx = i;
            sem_v(tbl->sessions_sem);
            return 0;
        }
    }

    sem_v(tbl->sessions_sem);
    log_error("session_open: session table full");
    return -1;
}

void session_close(SessionTable *tbl, int sess_idx) {
    if (sess_idx < 0 || sess_idx >= MAX_SESSIONS) return;
    if (sem_p(tbl->sessions_sem) < 0) return;
    memset(&tbl->sessions[sess_idx], 0, sizeof(Session));
    sem_v(tbl->sessions_sem);
}

pid_t session_get_pid(SessionTable *tbl, const char *username) {
    if (sem_p(tbl->sessions_sem) < 0) return 0;
    pid_t found = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (tbl->sessions[i].valid &&
            strcmp(tbl->sessions[i].username, username) == 0) {
            found = tbl->sessions[i].worker_pid;
            break;
        }
    }
    sem_v(tbl->sessions_sem);
    return found;
}

void session_set_cwd(SessionTable *tbl, int sess_idx, const char *cwd) {
    if (sess_idx < 0 || sess_idx >= MAX_SESSIONS) return;
    if (sem_p(tbl->sessions_sem) < 0) return;
    safe_strncpy(tbl->sessions[sess_idx].cwd, cwd, MAX_PATH);
    sem_v(tbl->sessions_sem);
}

void session_bg_inc(SessionTable *tbl, int sess_idx) {
    if (sess_idx < 0 || sess_idx >= MAX_SESSIONS) return;
    if (sem_p(tbl->sessions_sem) < 0) return;
    tbl->sessions[sess_idx].bg_count++;
    sem_v(tbl->sessions_sem);
}

void session_bg_dec(SessionTable *tbl, int sess_idx) {
    if (sess_idx < 0 || sess_idx >= MAX_SESSIONS) return;
    if (sem_p(tbl->sessions_sem) < 0) return;
    if (tbl->sessions[sess_idx].bg_count > 0)
        tbl->sessions[sess_idx].bg_count--;
    sem_v(tbl->sessions_sem);
}

int session_bg_count(SessionTable *tbl, int sess_idx) {
    if (sess_idx < 0 || sess_idx >= MAX_SESSIONS) return 0;
    if (sem_p(tbl->sessions_sem) < 0) return 0;
    int cnt = tbl->sessions[sess_idx].bg_count;
    sem_v(tbl->sessions_sem);
    return cnt;
}

/* ─────────────────────────────────────────────
 * session_dump_users — debug
 * ───────────────────────────────────────────── */
void session_dump_users(const SessionTable *tbl) {
    /* Nota: chiamare solo in contesto non-concurrent (es. startup debug) */
    log_info("=== User Table Dump ===");
    for (int i = 0; i < MAX_USERS; i++) {
        if (tbl->users[i].valid) {
            log_info("  [%d] user=%-16s uid=%d gid=%d home=%s",
                     i,
                     tbl->users[i].username,
                     (int)tbl->users[i].uid,
                     (int)tbl->users[i].gid,
                     tbl->users[i].home);
        }
    }
}

/* ─────────────────────────────────────────────
 * session_cleanup_dead
 *
 * Percorre la tabella delle sessioni e rimuove quelle il cui
 * worker_pid non esiste più (kill(pid, 0) == -1 con errno==ESRCH).
 * Chiamata periodicamente dal worker_loop per liberare slot.
 * ───────────────────────────────────────────── */
void session_cleanup_dead(SessionTable *tbl) {
    if (!tbl) return;

    if (sem_p(tbl->sessions_sem) < 0) return;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &tbl->sessions[i];
        if (!s->valid) continue;
        if (s->worker_pid <= 0) continue;

        if (kill(s->worker_pid, 0) < 0 && errno == ESRCH) {
            /* Il processo non esiste più: slot orfano */
            log_info("session_cleanup_dead: slot %d (user=%s pid=%d) orphan, freeing",
                     i, s->username, (int)s->worker_pid);
            memset(s, 0, sizeof(Session));
        }
    }

    sem_v(tbl->sessions_sem);
}
