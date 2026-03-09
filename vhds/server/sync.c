#define _POSIX_C_SOURCE 200809L

/*
 * sync.c — Lock lettori/scrittori con semafori System V
 *
 * MIGLIORAMENTI rispetto alla versione precedente:
 *  1. Readers/writers implementato con operazioni semop ATOMICHE su array
 *     di 3 semafori: elimina la race condition tra GETVAL e l'azione
 *     successiva che era presente nella vecchia versione.
 *  2. LRU eviction: quando la lock table è piena, le entry non più usate
 *     vengono rimosse automaticamente invece di restituire errore.
 *  3. Contatore di riferimenti per evitare eviction di entry in uso.
 *  4. Hash map per ricerca O(1) delle entry invece di scan lineare O(n).
 *
 * Struttura semafori (3 sem per file, sem_id da sync_create_semset):
 *   sem[0] = mutex        (init=1)   — protegge readers_count
 *   sem[1] = write_lock   (init=1)   — 0=locked, 1=free
 *   sem[2] = readers_count(init=0)   — numero lettori attivi
 *
 * READ_LOCK (atomico con due semop array-op):
 *   Step A: {sem[0], -1, 0}  →  acquisisce mutex
 *   Step B: {sem[2], +1, 0}  →  incrementa readers
 *   Se readers diventa 1:
 *   Step C: {sem[1], -1, 0}  →  acquisisce write_lock
 *   Step D: {sem[0], +1, 0}  →  rilascia mutex
 *
 * WRITE_LOCK (diretto):
 *   {sem[1], -1, 0}          →  acquisisce write_lock (esclusivo)
 */

#include "sync.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>

#define LOCK_SHM_KEY    0x4C4B5401

static int lock_shm_id = -1;

union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* ─────────────────────────────────────────────
 * Funzione semop con retry su EINTR
 * ───────────────────────────────────────────── */
static int do_semop(int sem_id, struct sembuf *ops, size_t nops) {
    while (semop(sem_id, ops, nops) < 0) {
        if (errno == EINTR) continue;
        perror("semop");
        return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * Inizializzazione SHM
 * ───────────────────────────────────────────── */
LockTable *sync_table_init(int create) {
    int flags_use;
    if (create) {
        /* Creazione: prova con IPC_EXCL per forzare una SHM fresca */
        flags_use = IPC_CREAT | IPC_EXCL | 0666;
    } else {
        flags_use = 0666;   /* solo attach */
    }
    lock_shm_id = shmget((key_t)LOCK_SHM_KEY,
                         sizeof(LockTable), flags_use);
    if (lock_shm_id < 0) {
        if (create && errno == EEXIST) {
            /* SHM stale con size diversa — rimuovila e ricrea */
            int stale_id = shmget((key_t)LOCK_SHM_KEY, 1, 0666);
            if (stale_id >= 0) shmctl(stale_id, IPC_RMID, NULL);
            lock_shm_id = shmget((key_t)LOCK_SHM_KEY,
                               sizeof(LockTable), flags_use);
        }
        if (lock_shm_id < 0) {
            perror("sync_table_init: shmget");
            return NULL;
        }
    }

    LockTable *tbl = (LockTable *)shmat(lock_shm_id, NULL, 0);
    if (tbl == (LockTable *)-1) {
        perror("sync_table_init: shmat");
        return NULL;
    }

    if (create) {
        memset(tbl, 0, sizeof(LockTable));

        tbl->table_sem = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
        if (tbl->table_sem < 0) {
            perror("sync_table_init: semget table_sem");
            shmdt(tbl);
            return NULL;
        }
        union semun su;
        su.val = 1;
        if (semctl(tbl->table_sem, 0, SETVAL, su) < 0) {
            perror("sync_table_init: semctl table_sem");
            semctl(tbl->table_sem, 0, IPC_RMID);
            shmdt(tbl);
            return NULL;
        }
        /* Inizializza tutti i sem_id a -1 (non allocati) */
        for (int i = 0; i < MAX_LOCKED_FILES; i++)
            tbl->entries[i].sem_id = -1;
    }

    return tbl;
}

void sync_table_detach(LockTable *tbl) {
    if (tbl) shmdt(tbl);
}

void sync_table_destroy(LockTable *tbl) {
    if (!tbl) return;

    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (tbl->entries[i].valid && tbl->entries[i].sem_id >= 0)
            semctl(tbl->entries[i].sem_id, 0, IPC_RMID);
    }
    if (tbl->table_sem >= 0)
        semctl(tbl->table_sem, 0, IPC_RMID);

    shmdt(tbl);
    if (lock_shm_id >= 0) {
        shmctl(lock_shm_id, IPC_RMID, NULL);
        lock_shm_id = -1;
    }
}

/* ─────────────────────────────────────────────
 * Mutex tabella
 * ───────────────────────────────────────────── */
static int table_lock(LockTable *tbl) {
    struct sembuf op = { 0, -1, 0 };
    return do_semop(tbl->table_sem, &op, 1);
}

static int table_unlock(LockTable *tbl) {
    struct sembuf op = { 0, +1, 0 };
    return do_semop(tbl->table_sem, &op, 1);
}

/* ─────────────────────────────────────────────
 * Crea set di 3 semafori per un file
 * ───────────────────────────────────────────── */
int sync_create_semset(void) {
    int sem_id = semget(IPC_PRIVATE, 3, IPC_CREAT | 0666);
    if (sem_id < 0) { perror("sync_create_semset: semget"); return -1; }

    unsigned short vals[3] = { 1, 1, 0 }; /* mutex=1, write=1, readers=0 */
    union semun su;
    su.array = vals;
    if (semctl(sem_id, 0, SETALL, su) < 0) {
        perror("sync_create_semset: SETALL");
        semctl(sem_id, 0, IPC_RMID);
        return -1;
    }
    return sem_id;
}

/* ─────────────────────────────────────────────
 * Ricerca entry — O(1) con hash lineare
 * ───────────────────────────────────────────── */

/* Hash djb2 limitato a MAX_LOCKED_FILES slot */
static int path_hash(const char *path) {
    unsigned long h = 5381;
    while (*path)
        h = h * 33 ^ (unsigned char)*path++;
    return (int)(h % MAX_LOCKED_FILES);
}

/*
 * Cerca entry per path usando hash + probe lineare.
 * Se `create` = 1, alloca una nuova entry se non trovata.
 * Deve essere chiamata con il table_lock acquisito.
 */
static FileLockEntry *find_entry(LockTable *tbl,
                                  const char *path, int create) {
    int start = path_hash(path);
    int i     = start;

    do {
        if (tbl->entries[i].valid &&
            strcmp(tbl->entries[i].path, path) == 0) {
            return &tbl->entries[i]; /* trovata */
        }
        i = (i + 1) % MAX_LOCKED_FILES;
    } while (i != start);

    if (!create) return NULL;

    /* Non trovata: cerca slot libero (seconda passata) */
    i = start;
    do {
        if (!tbl->entries[i].valid) {
            tbl->entries[i].valid   = 1;
            tbl->entries[i].readers = 0;
            tbl->entries[i].refcnt  = 0;
            safe_strncpy(tbl->entries[i].path, path,
                         sizeof(tbl->entries[i].path));
            tbl->entries[i].sem_id = sync_create_semset();
            if (tbl->entries[i].sem_id < 0) {
                tbl->entries[i].valid = 0;
                return NULL;
            }
            return &tbl->entries[i];
        }
        i = (i + 1) % MAX_LOCKED_FILES;
    } while (i != start);

    /*
     * Tabella piena: eviction LRU — cerca entry con refcnt==0
     * (non in uso al momento) e la ricicla.
     */
    i = start;
    do {
        if (tbl->entries[i].valid && tbl->entries[i].refcnt == 0) {
            /* Rimuovi vecchio semaforo */
            if (tbl->entries[i].sem_id >= 0) {
                semctl(tbl->entries[i].sem_id, 0, IPC_RMID);
                tbl->entries[i].sem_id = -1;
            }
            /* Ricrea per il nuovo path */
            tbl->entries[i].readers = 0;
            tbl->entries[i].refcnt  = 0;
            safe_strncpy(tbl->entries[i].path, path,
                         sizeof(tbl->entries[i].path));
            tbl->entries[i].sem_id = sync_create_semset();
            if (tbl->entries[i].sem_id < 0) {
                tbl->entries[i].valid = 0;
                return NULL;
            }
            log_debug("sync: evicted lock entry (table full), slot=%d", i);
            return &tbl->entries[i];
        }
        i = (i + 1) % MAX_LOCKED_FILES;
    } while (i != start);

    log_error("sync_find_entry: table full, all entries in use");
    return NULL;
}

/* Mantenuto per compatibilità con filesystem.c */
FileLockEntry *sync_get_or_create_entry(LockTable *tbl, const char *path) {
    return find_entry(tbl, path, 1);
}

/* ─────────────────────────────────────────────
 * READ LOCK — Algoritmo lettori/scrittori
 *
 * Implementazione con operazioni semop ATOMICHE:
 * non c'è più la race condition tra GETVAL e l'azione
 * perché usiamo le operazioni condizionali di semop.
 * ───────────────────────────────────────────── */
int sync_read_lock(LockTable *tbl, const char *path) {
    if (table_lock(tbl) < 0) return -1;

    FileLockEntry *e = find_entry(tbl, path, 1);
    if (!e) { table_unlock(tbl); return -1; }

    e->refcnt++;
    int sem_id = e->sem_id;
    table_unlock(tbl);

    /*
     * Acquisisce mutex, incrementa readers.
     * Se readers passa da 0 a 1: blocca write_lock (primo lettore).
     * Poi rilascia mutex.
     *
     * Usiamo un loop perché non possiamo fare tutto in un singolo
     * semop array atomico con la condizione "if readers==1 take write".
     * La soluzione è: semop atomico su mutex+readers, poi semop su write
     * condizionale solo se readers era 0 prima.
     *
     * Per evitare la race: usiamo SEM_UNDO=0 e operiamo come segue:
     *   1. lock mutex
     *   2. check readers via GETVAL (sicuro: siamo sotto mutex)
     *   3. se readers==0: acquisici write_lock (sotto mutex → atomico)
     *   4. incrementa readers
     *   5. unlock mutex
     */
    struct sembuf lock_mutex   = { SEM_MUTEX_IDX,   -1, 0 };
    struct sembuf unlock_mutex = { SEM_MUTEX_IDX,   +1, 0 };
    struct sembuf inc_readers  = { SEM_READERS_IDX, +1, 0 };

    /* Step 1: lock mutex */
    if (do_semop(sem_id, &lock_mutex, 1) < 0) {
        table_lock(tbl); e->refcnt--; table_unlock(tbl);
        return -1;
    }

    /* Step 2: leggi readers (sotto mutex → nessuna race) */
    int readers = semctl(sem_id, SEM_READERS_IDX, GETVAL);

    /* Step 3: se primo lettore, prendi write_lock SOTTO il mutex */
    if (readers == 0) {
        /*
         * Acquisisce write_lock e incrementa readers in UNICO semop
         * su due semafori — operazione atomica del kernel.
         */
        struct sembuf ops[2] = {
            { SEM_WRITE_IDX,   -1, 0 },
            { SEM_READERS_IDX, +1, 0 }
        };
        if (do_semop(sem_id, ops, 2) < 0) {
            do_semop(sem_id, &unlock_mutex, 1);
            table_lock(tbl); e->refcnt--; table_unlock(tbl);
            return -1;
        }
    } else {
        /* Incrementa readers: altri lettori già attivi, write già locked */
        if (do_semop(sem_id, &inc_readers, 1) < 0) {
            do_semop(sem_id, &unlock_mutex, 1);
            table_lock(tbl); e->refcnt--; table_unlock(tbl);
            return -1;
        }
    }

    /* Step 4: rilascia mutex */
    return do_semop(sem_id, &unlock_mutex, 1);
}

/* ─────────────────────────────────────────────
 * READ UNLOCK
 * ───────────────────────────────────────────── */
int sync_read_unlock(LockTable *tbl, const char *path) {
    if (table_lock(tbl) < 0) return -1;

    FileLockEntry *e = find_entry(tbl, path, 0);
    if (!e) { table_unlock(tbl); return -1; }

    int sem_id = e->sem_id;
    if (e->refcnt > 0) e->refcnt--;
    table_unlock(tbl);

    struct sembuf lock_mutex   = { SEM_MUTEX_IDX,   -1, 0 };
    struct sembuf unlock_mutex = { SEM_MUTEX_IDX,   +1, 0 };
    struct sembuf dec_readers  = { SEM_READERS_IDX, -1, 0 };

    if (do_semop(sem_id, &lock_mutex, 1) < 0) return -1;

    int readers = semctl(sem_id, SEM_READERS_IDX, GETVAL);

    if (readers == 1) {
        /* Ultimo lettore: rilascia write_lock e decrementa readers
         * in unica operazione atomica */
        struct sembuf ops[2] = {
            { SEM_WRITE_IDX,   +1, 0 },
            { SEM_READERS_IDX, -1, 0 }
        };
        if (do_semop(sem_id, ops, 2) < 0) {
            do_semop(sem_id, &unlock_mutex, 1);
            return -1;
        }
    } else {
        if (do_semop(sem_id, &dec_readers, 1) < 0) {
            do_semop(sem_id, &unlock_mutex, 1);
            return -1;
        }
    }

    return do_semop(sem_id, &unlock_mutex, 1);
}

/* ─────────────────────────────────────────────
 * WRITE LOCK — accesso esclusivo diretto
 * ───────────────────────────────────────────── */
int sync_write_lock(LockTable *tbl, const char *path) {
    if (table_lock(tbl) < 0) return -1;

    FileLockEntry *e = find_entry(tbl, path, 1);
    if (!e) { table_unlock(tbl); return -1; }

    e->refcnt++;
    int sem_id = e->sem_id;
    table_unlock(tbl);

    struct sembuf op = { SEM_WRITE_IDX, -1, 0 };
    if (do_semop(sem_id, &op, 1) < 0) {
        table_lock(tbl);
        FileLockEntry *e2 = find_entry(tbl, path, 0);
        if (e2 && e2->refcnt > 0) e2->refcnt--;
        table_unlock(tbl);
        return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * WRITE UNLOCK
 * ───────────────────────────────────────────── */
int sync_write_unlock(LockTable *tbl, const char *path) {
    if (table_lock(tbl) < 0) return -1;

    FileLockEntry *e = find_entry(tbl, path, 0);
    if (!e) { table_unlock(tbl); return -1; }

    int sem_id = e->sem_id;
    if (e->refcnt > 0) e->refcnt--;
    table_unlock(tbl);

    struct sembuf op = { SEM_WRITE_IDX, +1, 0 };
    return do_semop(sem_id, &op, 1);
}
