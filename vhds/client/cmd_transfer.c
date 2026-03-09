#define _POSIX_C_SOURCE 200809L

/*
 * cmd_transfer.c — Comandi di trasferimento file
 *
 *   upload, download, transfer_request, accept, reject
 */

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────
 * Helper: fork figlio per operazione background.
 * Il figlio esegue fn(sock, a, b, flags) e poi _exit().
 * Il padre incrementa bg_count e torna subito.
 *
 * Ritorna pid del figlio (>0), 0 se siamo nel figlio,
 * -1 su errore.
 * ───────────────────────────────────────────── */
typedef int (*transfer_fn)(int sock, const char *a,
                            const char *b, uint32_t flags);

static pid_t spawn_background(transfer_fn fn,
                               const char *a, const char *b) {
    g_client->bg_count++;
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork background");
        g_client->bg_count--;
        return -1;
    }

    if (pid == 0) {
        /* figlio: esegue il trasferimento, poi esce */
        fn(g_client->sock_fd, a, b, FLAG_BG);
        _exit(0);
    }

    printf("Background job started (pid %d)\n", (int)pid);
    return pid;
}

/* ─────────────────────────────────────────────
 * cmd_upload: upload [-b] <local> <remote>
 * ───────────────────────────────────────────── */
int cmd_upload(int argc, char **argv) {
    int bg = 0, local_i = 1, remote_i = 2;

    if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
        bg = 1; local_i = 2; remote_i = 3;
    }
    if (argc < remote_i + 1) {
        fprintf(stderr, "Usage: upload [-b] <local_path> <server_path>\n");
        return 0;
    }

    if (bg) {
        spawn_background(client_upload_file, argv[local_i], argv[remote_i]);
        return 0;
    }

    client_upload_file(g_client->sock_fd,
                       argv[local_i], argv[remote_i], FLAG_NONE);
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_download: download [-b] <remote> <local>
 * ───────────────────────────────────────────── */
int cmd_download(int argc, char **argv) {
    int bg = 0, remote_i = 1, local_i = 2;

    if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
        bg = 1; remote_i = 2; local_i = 3;
    }
    if (argc < local_i + 1) {
        fprintf(stderr, "Usage: download [-b] <server_path> <local_path>\n");
        return 0;
    }

    if (bg) {
        spawn_background(client_download_file, argv[remote_i], argv[local_i]);
        return 0;
    }

    client_download_file(g_client->sock_fd,
                         argv[remote_i], argv[local_i], FLAG_NONE);
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_transfer_request: transfer_request <file> <dest_user>
 *
 * Bloccante: attende che dest_user risponda (accept/reject).
 * Mostra spinner per segnalare l'attesa.
 * ───────────────────────────────────────────── */
int cmd_transfer_request(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: transfer_request <file> <dest_user>\n");
        return 0;
    }

    PayloadTransferReq p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.file,      argv[1], sizeof(p.file));
    safe_strncpy(p.dest_user, argv[2], sizeof(p.dest_user));
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_TRANSFER_REQ,
                 FLAG_NONE, &p, sizeof(p));

    printf("Waiting for \033[1m%s\033[0m to respond...\n", argv[2]);
    fflush(stdout);
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_accept: accept <directory> <ID>
 * ───────────────────────────────────────────── */
int cmd_accept(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: accept <directory> <ID>\n");
        return 0;
    }
    PayloadAccept p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.directory, argv[1], sizeof(p.directory));
    p.transfer_id = atoi(argv[2]);
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_ACCEPT,
                 FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_reject: reject <ID>
 * ───────────────────────────────────────────── */
int cmd_reject(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: reject <ID>\n");
        return 0;
    }
    PayloadReject p;
    memset(&p, 0, sizeof(p));
    p.transfer_id = atoi(argv[1]);
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_REJECT,
                 FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}
