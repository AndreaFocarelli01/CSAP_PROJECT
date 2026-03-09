#define _POSIX_C_SOURCE 200809L

#include "transfer.h"
#include "utils.h"
#include "network.h"
#include "filesystem.h"
#include "protocol.h"
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

#define TR_SHM_KEY  0x54520001  /* "TR\x00\x01" */

static int tr_shm_id = -1;

/* ─────────────────────────────────────────────
 * Stato globale del worker (per SIGUSR1 handler)
 * ───────────────────────────────────────────── */
static TransferTable *g_tr_tbl    = NULL;
static char           g_username[MAX_USERNAME];
static int            g_client_fd = -1;

/* union per semctl */
union semun_tr {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* ─────────────────────────────────────────────
 * Helper semaforo
 * ───────────────────────────────────────────── */
static int tr_sem_op(int sem_id, int delta) {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = (short)delta;
    op.sem_flg = 0;
    while (semop(sem_id, &op, 1) < 0) {
        if (errno == EINTR) continue;
        perror("tr_sem_op");
        return -1;
    }
    return 0;
}

static int tr_sem_create(int init_val) {
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("tr_sem_create"); return -1; }
    union semun_tr su;
    su.val = init_val;
    if (semctl(id, 0, SETVAL, su) < 0) {
        perror("tr_sem_create setval");
        semctl(id, 0, IPC_RMID);
        return -1;
    }
    return id;
}

static void tr_sem_destroy(int sem_id) {
    if (sem_id >= 0) semctl(sem_id, 0, IPC_RMID);
}

/* ─────────────────────────────────────────────
 * Mutex tabella transfer
 * ───────────────────────────────────────────── */
static int tbl_lock(TransferTable *tbl) {
    return tr_sem_op(tbl->table_sem, -1);
}
static int tbl_unlock(TransferTable *tbl) {
    return tr_sem_op(tbl->table_sem, +1);
}

/* ─────────────────────────────────────────────
 * Inizializzazione SHM
 * ───────────────────────────────────────────── */
TransferTable *transfer_table_init(int create) {
    int flags_use;
    if (create) {
        /* Creazione: prova con IPC_EXCL per forzare una SHM fresca */
        flags_use = IPC_CREAT | IPC_EXCL | 0666;
    } else {
        flags_use = 0666;   /* solo attach */
    }
    tr_shm_id = shmget((key_t)TR_SHM_KEY, sizeof(TransferTable), flags_use);
    if (tr_shm_id < 0) {
        if (create && errno == EEXIST) {
            int stale_id = shmget((key_t)TR_SHM_KEY, 1, 0666);
            if (stale_id >= 0) shmctl(stale_id, IPC_RMID, NULL);
            tr_shm_id = shmget((key_t)TR_SHM_KEY,
                               sizeof(TransferTable), flags_use);
        }
        if (tr_shm_id < 0) {
            perror("transfer_table_init shmget");
            return NULL;
        }
    }

    TransferTable *tbl = (TransferTable *)shmat(tr_shm_id, NULL, 0);
    if (tbl == (TransferTable *)-1) {
        perror("transfer_table_init shmat");
        return NULL;
    }

    if (create) {
        memset(tbl, 0, sizeof(TransferTable));
        tbl->next_id   = 1;
        tbl->table_sem = tr_sem_create(1);
        if (tbl->table_sem < 0) {
            shmdt(tbl);
            return NULL;
        }
    }

    return tbl;
}

void transfer_table_detach(TransferTable *tbl) {
    if (tbl) shmdt(tbl);
}

void transfer_table_destroy(TransferTable *tbl) {
    if (tbl == NULL) return;

    /* Distruggi semafori di ogni entry */
    for (int i = 0; i < MAX_TRANSFERS; i++) {
        if (tbl->entries[i].status != TR_FREE &&
            tbl->entries[i].notify_sem >= 0) {
            tr_sem_destroy(tbl->entries[i].notify_sem);
        }
    }
    if (tbl->table_sem >= 0) tr_sem_destroy(tbl->table_sem);

    shmdt(tbl);
    if (tr_shm_id >= 0) {
        shmctl(tr_shm_id, IPC_RMID, NULL);
        tr_shm_id = -1;
    }
}

/* ─────────────────────────────────────────────
 * Trova entry per ID
 * Richiede che la tabella sia già lockata.
 * ───────────────────────────────────────────── */
static TransferEntry *find_by_id(TransferTable *tbl, int id) {
    for (int i = 0; i < MAX_TRANSFERS; i++) {
        if (tbl->entries[i].status != TR_FREE &&
            tbl->entries[i].id == id) {
            return &tbl->entries[i];
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────
 * transfer_worker_init
 * Salva stato globale per l'handler SIGUSR1.
 * ───────────────────────────────────────────── */
void transfer_worker_init(TransferTable *tr_tbl,
                          const char *username,
                          int client_fd) {
    g_tr_tbl    = tr_tbl;
    g_client_fd = client_fd;
    safe_strncpy(g_username, username, MAX_USERNAME);
}

/* ─────────────────────────────────────────────
 * transfer_check_pending
 * Chiamata dopo login: notifica transfer PENDING
 * per questo utente (caso "destinatario era offline").
 * ───────────────────────────────────────────── */
void transfer_check_pending(TransferTable *tr_tbl,
                             const char *username,
                             int client_fd) {
    if (tr_tbl == NULL) return;
    if (tbl_lock(tr_tbl) < 0) return;

    for (int i = 0; i < MAX_TRANSFERS; i++) {
        TransferEntry *e = &tr_tbl->entries[i];
        if (e->status == TR_PENDING &&
            strcmp(e->dst_user, username) == 0) {

            /* Aggiorna pid destinatario */
            e->dst_pid = getpid();

            /* Notifica client */
            PayloadTransferNotify ntf;
            ntf.transfer_id = e->id;
            safe_strncpy(ntf.src_user, e->src_user, MAX_USERNAME);
            safe_strncpy(ntf.filename, e->file_path, MAX_PATH);

            /* Invia fuori dal lock per evitare deadlock su send bloccante */
            tbl_unlock(tr_tbl);
            net_send_msg(client_fd, (uint32_t)RES_TRANSFER_REQ,
                         FLAG_NONE, &ntf, sizeof(ntf));
            if (tbl_lock(tr_tbl) < 0) return;
        }
    }

    tbl_unlock(tr_tbl);
}

/* ─────────────────────────────────────────────
 * SIGUSR1 handler (async-signal-safe)
 * Scrive su una pipe auto-pipe per notificare
 * il loop principale del worker.
 * NOTA: usiamo una variabile volatile sig_atomic_t
 * per non fare I/O nel signal handler.
 * ───────────────────────────────────────────── */
static volatile sig_atomic_t g_sigusr1_pending = 0;

void transfer_sigusr1_handler(int sig) {
    (void)sig;
    g_sigusr1_pending = 1;
}

/*
 * Da chiamare nel loop comandi del worker dopo ogni recv:
 * se g_sigusr1_pending è settato, chiama transfer_check_pending.
 */
void transfer_process_pending_signal(void) {
    if (g_sigusr1_pending && g_tr_tbl != NULL) {
        g_sigusr1_pending = 0;
        transfer_check_pending(g_tr_tbl, g_username, g_client_fd);
    }
}

/* ─────────────────────────────────────────────
 * transfer_request
 * ───────────────────────────────────────────── */
int transfer_request(const char *vfs_root,
                     Session *src_sess,
                     SessionTable *sess_tbl,
                     LockTable *lock_tbl,
                     TransferTable *tr_tbl,
                     int client_fd,
                     const char *file_vfs,
                     const char *dest_user) {
    char abs_file[MAX_PATH];

    /* Risolvi path del file */
    if (path_resolve(vfs_root, src_sess->cwd,
                     file_vfs, abs_file, sizeof(abs_file)) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: invalid file path");
        return -1;
    }

    /* Verifica che il destinatario esista */
    const UserRecord *dst_ur = session_find_user(sess_tbl, dest_user);
    if (dst_ur == NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: destination user not found");
        return -1;
    }

    /* Verifica che il file esista e sia leggibile */
    struct stat st;
    if (stat(abs_file, &st) < 0 || !S_ISREG(st.st_mode)) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: source file not found");
        return -1;
    }

    /* Alloca entry nella tabella */
    if (tbl_lock(tr_tbl) < 0) return -1;

    TransferEntry *slot = NULL;
    for (int i = 0; i < MAX_TRANSFERS; i++) {
        if (tr_tbl->entries[i].status == TR_FREE) {
            slot = &tr_tbl->entries[i];
            break;
        }
    }

    if (slot == NULL) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer table full");
        return -1;
    }

    int tr_id = tr_tbl->next_id++;
    slot->id         = tr_id;
    slot->status     = TR_PENDING;
    slot->src_pid    = getpid();
    slot->dst_pid    = 0;
    slot->dst_dir[0] = '\0';
    safe_strncpy(slot->src_user,  src_sess->username, MAX_USERNAME);
    safe_strncpy(slot->dst_user,  dest_user,          MAX_USERNAME);
    safe_strncpy(slot->file_path, abs_file,            MAX_PATH);

    slot->notify_sem = tr_sem_create(0);   /* bloccante: parte da 0 */
    if (slot->notify_sem < 0) {
        slot->status = TR_FREE;
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot create semaphore");
        return -1;
    }

    /* Trova pid destinatario (se online) */
    pid_t dst_pid = session_get_pid(sess_tbl, dest_user);
    if (dst_pid > 0) {
        slot->dst_pid = dst_pid;
        tbl_unlock(tr_tbl);

        /* Notifica destinatario tramite SIGUSR1 */
        kill(dst_pid, SIGUSR1);
    } else {
        tbl_unlock(tr_tbl);
        /* Destinatario offline: aspetterà */
        log_info("transfer_request: dst user '%s' is offline, waiting",
                 dest_user);
    }

    /* Informa il mittente dell'ID assegnato */
    char msg[64];
    snprintf(msg, sizeof(msg), "OK Transfer ID: %d (waiting for response)",
             tr_id);
    net_send_response(client_fd, RES_OK, msg);

    /* Attesa bloccante sul semaforo di notifica */
    if (tr_sem_op(slot->notify_sem, -1) < 0) {
        /* Interrotto: pulisci e ritorna */
        tbl_lock(tr_tbl);
        slot->status = TR_FREE;
        tr_sem_destroy(slot->notify_sem);
        slot->notify_sem = -1;
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer interrupted");
        return -1;
    }

    /* Leggi lo stato aggiornato dal destinatario */
    tbl_lock(tr_tbl);
    TransferEntry *e = find_by_id(tr_tbl, tr_id);
    if (e == NULL) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer entry lost");
        return -1;
    }

    TransferStatus final_status = e->status;
    char dst_dir_copy[MAX_PATH];
    safe_strncpy(dst_dir_copy, e->dst_dir, MAX_PATH);

    tbl_unlock(tr_tbl);

    if (final_status == TR_REJECTED) {
        /* Cleanup semaforo */
        tbl_lock(tr_tbl);
        if (find_by_id(tr_tbl, tr_id)) {
            tr_sem_destroy(e->notify_sem);
            e->status = TR_FREE;
        }
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_TRANSFER_REJECT,
                          "Transfer rejected by destination user");
        return -1;
    }

    if (final_status != TR_ACCEPTED) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: unexpected transfer state");
        return -1;
    }

    /* Costruisce path destinazione */
    char abs_dst_dir[MAX_PATH];
    if (path_resolve(vfs_root, dst_ur->home,
                     dst_dir_copy, abs_dst_dir, sizeof(abs_dst_dir)) < 0) {
        tbl_lock(tr_tbl);
        if (find_by_id(tr_tbl, tr_id)) {
            tr_sem_destroy(e->notify_sem);
            e->status = TR_FREE;
        }
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: invalid destination path");
        return -1;
    }

    char basename[MAX_PATH];
    path_basename(abs_file, basename, sizeof(basename));

    char abs_dst_file[MAX_PATH];
    safe_strncpy(abs_dst_file, abs_dst_dir, sizeof(abs_dst_file));
    strncat(abs_dst_file, "/", sizeof(abs_dst_file) - strlen(abs_dst_file) - 1);
    strncat(abs_dst_file, basename, sizeof(abs_dst_file) - strlen(abs_dst_file) - 1);

    /* Acquisisce lock lettura su sorgente, scrittura su destinazione */
    sync_read_lock(lock_tbl, abs_file);
    sync_write_lock(lock_tbl, abs_dst_file);

    int copy_ret = fs_copy_raw(abs_file, abs_dst_file);

    sync_write_unlock(lock_tbl, abs_dst_file);
    sync_read_unlock(lock_tbl, abs_file);

    /* Aggiorna stato entry */
    tbl_lock(tr_tbl);
    TransferEntry *ef = find_by_id(tr_tbl, tr_id);
    if (ef) {
        ef->status = (copy_ret == 0) ? TR_DONE : TR_FREE;
        tr_sem_destroy(ef->notify_sem);
        ef->notify_sem = -1;
        if (ef->status == TR_DONE) ef->status = TR_FREE;
    }
    tbl_unlock(tr_tbl);

    if (copy_ret < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: file copy failed");
        return -1;
    }

    net_send_response(client_fd, RES_TRANSFER_DONE,
                      "Transfer completed successfully");
    return 0;
}

/* ─────────────────────────────────────────────
 * transfer_accept
 * ───────────────────────────────────────────── */
int transfer_accept(TransferTable *tr_tbl,
                    SessionTable *sess_tbl,
                    int client_fd,
                    int tr_id,
                    const char *dst_dir,
                    const char *dst_username) {
    (void)sess_tbl;

    if (tbl_lock(tr_tbl) < 0) return -1;

    TransferEntry *e = find_by_id(tr_tbl, tr_id);
    if (e == NULL) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer ID not found");
        return -1;
    }

    if (strcmp(e->dst_user, dst_username) != 0) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: not your transfer to accept");
        return -1;
    }

    if (e->status != TR_PENDING) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer not in PENDING state");
        return -1;
    }

    safe_strncpy(e->dst_dir, dst_dir, MAX_PATH);
    e->dst_pid = getpid();
    e->status  = TR_ACCEPTED;

    int notify_sem = e->notify_sem;
    tbl_unlock(tr_tbl);

    /* Sveglia il worker mittente */
    tr_sem_op(notify_sem, +1);

    net_send_response(client_fd, RES_OK,
                      "OK Transfer accepted, file incoming...");
    return 0;
}

/* ─────────────────────────────────────────────
 * transfer_reject
 * ───────────────────────────────────────────── */
int transfer_reject(TransferTable *tr_tbl,
                    int client_fd,
                    int tr_id,
                    const char *dst_username) {
    if (tbl_lock(tr_tbl) < 0) return -1;

    TransferEntry *e = find_by_id(tr_tbl, tr_id);
    if (e == NULL) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: transfer ID not found");
        return -1;
    }

    if (strcmp(e->dst_user, dst_username) != 0) {
        tbl_unlock(tr_tbl);
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: not your transfer to reject");
        return -1;
    }

    e->status = TR_REJECTED;
    int notify_sem = e->notify_sem;
    tbl_unlock(tr_tbl);

    /* Sveglia il worker mittente */
    tr_sem_op(notify_sem, +1);

    net_send_response(client_fd, RES_OK, "OK Transfer rejected");
    return 0;
}

/* ─────────────────────────────────────────────
 * transfer_cleanup_expired
 * Rimuove entry PENDING che non hanno ricevuto risposta entro
 * TR_TIMEOUT_SEC. Chiamata periodicamente dal worker loop.
 * ───────────────────────────────────────────── */
void transfer_cleanup_expired(TransferTable *tbl) {
    if (!tbl) return;

    time_t now = time(NULL);

    if (tbl_lock(tbl) < 0) return;

    for (int i = 0; i < MAX_TRANSFERS; i++) {
        TransferEntry *e = &tbl->entries[i];
        if (e->status != TR_PENDING) continue;
        if (e->created_at == 0) continue;

        double age = difftime(now, e->created_at);
        if (age < (double)TR_TIMEOUT_SEC) continue;

        log_info("transfer: entry id=%d expired (age=%.0fs), cleaning up",
                 e->id, age);

        /* Sveglia il sorgente con errore (se ancora in attesa) */
        if (e->notify_sem >= 0) {
            tr_sem_op(e->notify_sem, +1);   /* sblocca semop(-1) nel src */
            tr_sem_destroy(e->notify_sem);
            e->notify_sem = -1;
        }

        memset(e, 0, sizeof(TransferEntry));
    }

    tbl_unlock(tbl);
}
