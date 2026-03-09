#ifndef TRANSFER_H
#define TRANSFER_H

#include <sys/types.h>
#include <time.h>
#include "protocol.h"
#include "session.h"
#include "sync.h"

#define MAX_TRANSFERS   64

/* ─────────────────────────────────────────────
 * Stato di un trasferimento P2P
 * ───────────────────────────────────────────── */
typedef enum {
    TR_FREE     = 0,
    TR_PENDING  = 1,   /* in attesa di risposta dal destinatario */
    TR_ACCEPTED = 2,   /* destinatario ha accettato              */
    TR_REJECTED = 3,   /* destinatario ha rifiutato              */
    TR_DONE     = 4    /* file copiato con successo              */
} TransferStatus;

/* ─────────────────────────────────────────────
 * Entry trasferimento (in Shared Memory)
 * ───────────────────────────────────────────── */
#define TR_TIMEOUT_SEC  120     /* entry PENDING scadono dopo 120s senza risposta */

typedef struct {
    TransferStatus status;
    int            id;
    char           src_user[MAX_USERNAME];
    char           dst_user[MAX_USERNAME];
    char           file_path[MAX_PATH];     /* path assoluto nel VFS       */
    char           dst_dir[MAX_PATH];       /* directory destinazione (VFS)*/
    pid_t          src_pid;                 /* pid worker mittente          */
    pid_t          dst_pid;                 /* pid worker destinatario      */
    int            notify_sem;              /* sem System V per wake-up src */
    time_t         created_at;             /* timestamp creazione entry    */
} TransferEntry;

/* ─────────────────────────────────────────────
 * Tabella trasferimenti (in Shared Memory)
 * ───────────────────────────────────────────── */
typedef struct {
    TransferEntry entries[MAX_TRANSFERS];
    int           next_id;      /* counter atomico per ID univoci */
    int           table_sem;    /* mutex sulla tabella            */
} TransferTable;

/* ─────────────────────────────────────────────
 * Inizializzazione
 * ───────────────────────────────────────────── */
TransferTable *transfer_table_init(int create);
void           transfer_table_detach(TransferTable *tbl);
void           transfer_table_destroy(TransferTable *tbl);

/* ─────────────────────────────────────────────
 * API lato server (chiamata dal worker)
 * ───────────────────────────────────────────── */

/*
 * Gestisce il comando transfer_request dal client mittente.
 * - Crea entry in SHM con status TR_PENDING.
 * - Se il destinatario è online, manda SIGUSR1 al suo worker.
 * - Se offline, resta in attesa bloccata (semop sul notify_sem).
 * - Quando si sblocca, controlla lo stato e copia o segnala rifiuto.
 *
 * `vfs_root`   : root assoluta del VFS
 * `src_sess`   : sessione del mittente
 * `sess_tbl`   : tabella sessioni
 * `lock_tbl`   : tabella lock
 * `client_fd`  : socket verso il mittente
 * `file_vfs`   : path del file nel VFS (relativo alla home mittente)
 * `dest_user`  : username destinatario
 */
int transfer_request(const char *vfs_root,
                     Session *src_sess,
                     SessionTable *sess_tbl,
                     LockTable *lock_tbl,
                     TransferTable *tr_tbl,
                     int client_fd,
                     const char *file_vfs,
                     const char *dest_user);

/*
 * Gestisce il comando accept <directory> <ID> dal client destinatario.
 * - Trova entry per `tr_id`.
 * - Imposta dst_dir e status = TR_ACCEPTED.
 * - Sveglia il worker mittente (semop +1).
 *
 * Ritorna 0 su successo, -1 su errore (es. ID non trovato).
 */
int transfer_accept(TransferTable *tr_tbl,
                    SessionTable *sess_tbl,
                    int client_fd,
                    int tr_id,
                    const char *dst_dir,
                    const char *dst_username);

/*
 * Gestisce il comando reject <ID> dal client destinatario.
 * Imposta status = TR_REJECTED e sveglia il worker mittente.
 */
int transfer_reject(TransferTable *tr_tbl,
                    int client_fd,
                    int tr_id,
                    const char *dst_username);

/*
 * Handler SIGUSR1: chiamato nel worker destinatario quando arriva
 * una transfer_request. Legge le entry PENDING per l'utente corrente
 * e le notifica al client.
 * Usa solo funzioni async-signal-safe.
 */
void transfer_sigusr1_handler(int sig);

/*
 * Inizializza il worker destinatario: salva i puntatori globali
 * necessari all'handler SIGUSR1.
 */
void transfer_worker_init(TransferTable *tr_tbl,
                          const char *username,
                          int client_fd);

/*
 * Cerca trasferimenti PENDING per `username` e li notifica al client.
 * Da chiamare dopo il login per recuperare richieste arrivate offline.
 */
void transfer_check_pending(TransferTable *tr_tbl,
                             const char *username,
                             int client_fd);


/*
 * Da chiamare nel loop comandi del worker dopo ogni recv:
 * processa il segnale SIGUSR1 pendente (se presente).
 */
void transfer_process_pending_signal(void);
/* ─────────────────────────────────────────────
 * API lato CLIENT (client/transfer.c)
 * ───────────────────────────────────────────── */

/*
 * Invia file locale `local_path` al server → `server_path`.
 * `flags`: FLAG_NONE o FLAG_BG (gestito dal chiamante).
 * Ritorna 0 su successo, -1 su errore.
 */
int client_upload_file(int sock_fd,
                       const char *local_path,
                       const char *server_path,
                       uint32_t flags);

/*
 * Scarica `server_path` dal server e lo salva in `local_path`.
 * Ritorna 0 su successo, -1 su errore.
 */
int client_download_file(int sock_fd,
                         const char *server_path,
                         const char *local_path,
                         uint32_t flags);



/* Rimuove entry PENDING scadute (chiamata periodicamente dal worker) */
void transfer_cleanup_expired(TransferTable *tbl);

#endif /* TRANSFER_H */
