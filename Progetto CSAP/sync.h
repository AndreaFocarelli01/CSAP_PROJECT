#ifndef SYNC_H
#define SYNC_H

#include <sys/types.h>

#define MAX_LOCKED_FILES    256
#define SEM_MUTEX_IDX       0   /* sem[0]: mutex sul contatore lettori */
#define SEM_WRITE_IDX       1   /* sem[1]: write lock (0=locked,1=free)*/
#define SEM_READERS_IDX     2   /* sem[2]: contatore lettori attivi    */

/* ─────────────────────────────────────────────
 * Entry nella tabella lock (in Shared Memory)
 * ───────────────────────────────────────────── */
typedef struct {
    int  valid;             /* 1 = slot in uso                     */
    char path[512];         /* path canonico del file (chiave)     */
    int  sem_id;            /* ID semaforo System V (set da 3 sem) */
    int  readers;           /* contatore lettori (ridondante/debug)*/
    int  refcnt;            /* riferimenti attivi — protegge da eviction LRU */
} FileLockEntry;

/* ─────────────────────────────────────────────
 * Tabella lock condivisa (in Shared Memory)
 * ───────────────────────────────────────────── */
typedef struct {
    FileLockEntry entries[MAX_LOCKED_FILES];
    int           table_sem;    /* semaforo mutex sulla tabella stessa */
} LockTable;

/* ─────────────────────────────────────────────
 * Inizializzazione
 * ───────────────────────────────────────────── */

/*
 * Alloca/collega la LockTable in shared memory.
 * `create` = 1 → crea (server padre), 0 → collega (worker).
 * Ritorna puntatore, NULL su errore.
 */
LockTable *sync_table_init(int create);

/* Distacca dal processo corrente. */
void       sync_table_detach(LockTable *tbl);

/* Distrugge tutta la SHM e i semafori (solo padre alla fine). */
void       sync_table_destroy(LockTable *tbl);

/* ─────────────────────────────────────────────
 * Lock lettori / scrittori
 * ───────────────────────────────────────────── */

/*
 * Acquisisce il lock in lettura su `path`.
 * Più lettori concorrenti sono ammessi.
 * Blocca se c'è uno scrittore attivo.
 * Ritorna 0 su successo, -1 su errore.
 */
int sync_read_lock  (LockTable *tbl, const char *path);

/*
 * Rilascia il lock in lettura su `path`.
 * Ritorna 0 su successo, -1 su errore.
 */
int sync_read_unlock(LockTable *tbl, const char *path);

/*
 * Acquisisce il lock in scrittura su `path`.
 * Accesso esclusivo: blocca se ci sono lettori o altri scrittori.
 * Ritorna 0 su successo, -1 su errore.
 */
int sync_write_lock  (LockTable *tbl, const char *path);

/*
 * Rilascia il lock in scrittura su `path`.
 * Ritorna 0 su successo, -1 su errore.
 */
int sync_write_unlock(LockTable *tbl, const char *path);

/* ─────────────────────────────────────────────
 * Funzioni interne (usate da sync.c)
 * ───────────────────────────────────────────── */

/*
 * Trova o crea una entry per `path` nella tabella.
 * Ritorna puntatore alla entry, NULL su errore/tabella piena.
 * La tabella deve essere già lockata (table_sem) dal chiamante.
 */
FileLockEntry *sync_get_or_create_entry(LockTable *tbl, const char *path);

/*
 * Crea un semaforo System V con 3 valori inizializzati.
 * Ritorna sem_id, -1 su errore.
 */
int sync_create_semset(void);

#endif /* SYNC_H */
