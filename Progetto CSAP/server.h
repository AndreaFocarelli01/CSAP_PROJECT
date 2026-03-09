#ifndef SERVER_H
#define SERVER_H

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "protocol.h"
#include "session.h"
#include "sync.h"
#include "transfer.h"
#include "utils.h"
#include <signal.h>

#include <sys/types.h>

/* ─────────────────────────────────────────────
 * Stato globale del server (unica istanza in G)
 * ───────────────────────────────────────────── */
typedef struct {
    char           vfs_root[MAX_PATH];
    char           ip[64];
    int            port;
    int            listen_fd;
    SessionTable  *sess_tbl;
    LockTable     *lock_tbl;
    TransferTable *tr_tbl;
    gid_t          common_gid;
} ServerState;

extern ServerState G;
extern volatile sig_atomic_t g_shutdown;

/* ─────────────────────────────────────────────
 * server_init.c — inizializzazione e lifecycle
 * ───────────────────────────────────────────── */
int  server_init(int argc, char *argv[]);
void server_accept_loop(void);
void server_shutdown(void);

/* ─────────────────────────────────────────────
 * server_signals.c — gestione segnali
 * ───────────────────────────────────────────── */
void signals_init_parent(void);
void signals_init_worker(void);

/* ─────────────────────────────────────────────
 * server_user.c — create_user, login
 * ───────────────────────────────────────────── */
int cmd_create_user(int client_fd, const char *username,
                    const char *perm_str);
int cmd_login(int client_fd, const char *username,
              int *sess_idx_out, Session **sess_out);

/* ─────────────────────────────────────────────
 * server_dispatch.c — dispatch + worker loop
 * ───────────────────────────────────────────── */
void dispatch(int client_fd, Session *sess, int sess_idx,
              MsgHeader *hdr, void *payload);
void worker_loop(int client_fd);

#endif /* SERVER_H */
