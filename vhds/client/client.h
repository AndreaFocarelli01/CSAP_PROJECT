#ifndef CLIENT_H
#define CLIENT_H

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "protocol.h"
#include "network.h"
#include "transfer.h"
#include "utils.h"

#include <stdint.h>
#include <signal.h>

/* ─────────────────────────────────────────────
 * Costanti client
 * ───────────────────────────────────────────── */
#define INPUT_BUFSIZE   (MAX_PATH * 2 + 64)
#define WRITE_CHUNK     4096

/* ─────────────────────────────────────────────
 * Stato globale del client (unica istanza in main_client.c)
 * Tutti gli altri moduli vi accedono tramite puntatore.
 * ───────────────────────────────────────────── */
typedef struct {
    int  sock_fd;
    int  logged_in;
    char username[MAX_USERNAME];
    char cwd[MAX_PATH];
    char server_addr[64];           /* es. "127.0.0.1:9494" — per il banner */
    volatile sig_atomic_t bg_count;
} ClientState;

/* Puntatore globale allo stato: inizializzato in main, letto ovunque */
extern ClientState *g_client;

/* ─────────────────────────────────────────────
 * Interfacce dei moduli
 * ───────────────────────────────────────────── */

/* notify.c */
void drain_async_notifications(void);
int  handle_response(void);
int  recv_data_stream_to_stdout(void);

/* history.c */
void        history_init(void);
void        history_save(void);
void        history_add(const char *cmd);
void        history_print(void);
const char *history_expand(const char *cmd);

/* cmd_auth.c */
int cmd_login(int argc, char **argv);
int cmd_create_user(int argc, char **argv);
int cmd_exit(int argc, char **argv);
int cmd_ping(int argc, char **argv);
int cmd_help(int argc, char **argv);

/* cmd_fs.c */
int cmd_create(int argc, char **argv);
int cmd_chmod(int argc, char **argv);
int cmd_move(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_delete(int argc, char **argv);
int cmd_read(int argc, char **argv);
int cmd_write(int argc, char **argv);

/* cmd_transfer.c */
int cmd_upload(int argc, char **argv);
int cmd_download(int argc, char **argv);
int cmd_transfer_request(int argc, char **argv);
int cmd_accept(int argc, char **argv);
int cmd_reject(int argc, char **argv);

#endif /* CLIENT_H */
