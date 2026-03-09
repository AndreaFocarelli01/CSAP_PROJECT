#define _POSIX_C_SOURCE 200809L

/*
 * cmd_fs.c — Comandi filesystem
 *
 *   create, chmod, move, cd, list, delete, read, write
 */

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ─────────────────────────────────────────────
 * Helper: invia un comando con un singolo path
 * ───────────────────────────────────────────── */
static int send_path_cmd(uint32_t cmd, const char *path) {
    PayloadPath p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.path, path, sizeof(p.path));
    return net_send_msg(g_client->sock_fd, cmd, FLAG_NONE, &p, sizeof(p));
}

/* ─────────────────────────────────────────────
 * Helper: legge il PRIMO messaggio di uno stream RES_DATA.
 * Se è RES_ERROR, lo stampa e ritorna -1.
 * Se è RES_DATA, stampa il chunk e chiama recv_data_stream_to_stdout.
 * Se è RES_DATA_END, non c'è niente da leggere (ritorna 0 silenzioso).
 * ───────────────────────────────────────────── */
static int recv_stream_with_error_check(int sock) {
    MsgHeader hdr;
    char first[sizeof(PayloadDataChunk) + 16];

    if (net_recv_msg(sock, &hdr, first, sizeof(first)) < 0) {
        fprintf(stderr, "connection error\n");
        return -1;
    }

    if (hdr.cmd == (uint32_t)RES_ERROR) {
        size_t plen = hdr.payload_len < sizeof(first)
                      ? hdr.payload_len : sizeof(first) - 1;
        first[plen] = '\0';
        fprintf(stderr, "%s\n", first);
        return -1;
    }

    if (hdr.cmd == (uint32_t)RES_DATA_END)
        return 0;   /* stream vuoto */

    if (hdr.cmd == (uint32_t)RES_DATA &&
        hdr.payload_len >= (uint32_t)sizeof(uint32_t)) {
        uint32_t clen;
        memcpy(&clen, first, sizeof(uint32_t));
        clen = ntohl(clen);
        uint32_t avail = hdr.payload_len - (uint32_t)sizeof(uint32_t);
        if (clen > avail) clen = avail;
        if (clen > 0)
            write_all(STDOUT_FILENO, first + sizeof(uint32_t), clen);
    }

    return recv_data_stream_to_stdout();
}

/* ─────────────────────────────────────────────
 * Helper: estrae -offset=N da argv[i]
 * Ritorna 1 se trovato, 0 altrimenti.
 * ───────────────────────────────────────────── */
static int parse_offset(const char *arg, int *out) {
    if (strncmp(arg, "-offset=", 8) == 0) {
        *out = atoi(arg + 8);
        return 1;
    }
    return 0;
}

static int parse_max_bytes(const char *arg, int *out) {
    if (strncmp(arg, "-n=", 3) == 0) {
        *out = atoi(arg + 3);
        return 1;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_create: create [-d] <path> <perm>
 * ───────────────────────────────────────────── */
int cmd_create(int argc, char **argv) {
    int is_dir = 0, path_i = 1, perm_i = 2;

    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        is_dir = 1; path_i = 2; perm_i = 3;
    }
    if (argc < perm_i + 1) {
        fprintf(stderr, "Usage: create [-d] <path> <permissions>\n");
        return 0;
    }

    PayloadCreate p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.path,        argv[path_i], sizeof(p.path));
    safe_strncpy(p.permissions, argv[perm_i], sizeof(p.permissions));
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_CREATE,
                 is_dir ? FLAG_DIR : FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_chmod: chmod <path> <perm>
 * ───────────────────────────────────────────── */
int cmd_chmod(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: chmod <path> <permissions>\n");
        return 0;
    }
    PayloadChmod p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.path,        argv[1], sizeof(p.path));
    safe_strncpy(p.permissions, argv[2], sizeof(p.permissions));
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_CHMOD,
                 FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_move: move <src> <dst>
 * ───────────────────────────────────────────── */
int cmd_move(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: move <src> <dst>\n");
        return 0;
    }
    PayloadMove p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.src, argv[1], sizeof(p.src));
    safe_strncpy(p.dst, argv[2], sizeof(p.dst));
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_MOVE,
                 FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_cd: cd <path>
 *
 * Il server risponde "OK <new_cwd>"; aggiorniamo g_client->cwd.
 * ───────────────────────────────────────────── */
int cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cd <path>\n");
        return 0;
    }

    int sock = g_client->sock_fd;
    send_path_cmd((uint32_t)CMD_CD, argv[1]);

    MsgHeader hdr;
    char resp[MAX_RESPONSE];
    if (net_recv_msg(sock, &hdr, resp, sizeof(resp)) != 0) {
        fprintf(stderr, "cd: connection error\n");
        return 0;
    }
    size_t plen = hdr.payload_len < sizeof(resp) ? hdr.payload_len : sizeof(resp)-1;
    resp[plen] = '\0';

    if (hdr.cmd == (uint32_t)RES_OK) {
        /* Risposta: "OK <new_vfs_cwd>" — estrae la parte dopo il primo spazio */
        char *sp = strchr(resp, ' ');
        if (sp) safe_strncpy(g_client->cwd, sp + 1, sizeof(g_client->cwd));
        printf("%s\n", resp);
    } else {
        fprintf(stderr, "%s\n", resp);
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_list: list [path]
 *
 * Il server risponde con stream RES_DATA oppure RES_ERROR.
 * ───────────────────────────────────────────── */
int cmd_list(int argc, char **argv) {
    const char *lpath = (argc >= 2) ? argv[1] : ".";
    int   sock = g_client->sock_fd;

    (void)sock;
    send_path_cmd((uint32_t)CMD_LIST, lpath);
    recv_stream_with_error_check(sock);
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_delete: delete <path>
 * ───────────────────────────────────────────── */
int cmd_delete(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: delete <path>\n");
        return 0;
    }
    send_path_cmd((uint32_t)CMD_DELETE, argv[1]);
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_read: read [-offset=N] <path>
 * ───────────────────────────────────────────── */
int cmd_read(int argc, char **argv) {
    int offset    = -1;
    int max_bytes = 0;
    int path_i    = 1;

    /* Parsing argomenti opzionali prima del path */
    while (path_i < argc && argv[path_i][0] == '-') {
        if (parse_offset(argv[path_i], &offset))    { path_i++; continue; }
        if (parse_max_bytes(argv[path_i], &max_bytes)) { path_i++; continue; }
        break;  /* flag sconosciuto → trattato come path */
    }

    if (argc < path_i + 1) {
        fprintf(stderr, "Usage: read [-offset=N] [-n=N] <path>\n");
        return 0;
    }

    int sock = g_client->sock_fd;
    PayloadRead p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.path, argv[path_i], sizeof(p.path));
    p.offset    = offset;
    p.max_bytes = max_bytes;
    net_send_msg(sock, (uint32_t)CMD_READ, FLAG_NONE, &p, sizeof(p));

    if (recv_stream_with_error_check(sock) == 0)
        printf("\n");
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_write: write [-offset=N] <path>
 *
 * Legge da stdin riga per riga fino a "." solo.
 * ───────────────────────────────────────────── */
int cmd_write(int argc, char **argv) {
    int offset     = -1;
    int append_mode = 0;
    int path_i     = 1;

    /* Parsing argomenti opzionali */
    while (path_i < argc && argv[path_i][0] == '-') {
        if (strcmp(argv[path_i], "-a") == 0) {
            append_mode = 1; path_i++; continue;
        }
        if (parse_offset(argv[path_i], &offset)) { path_i++; continue; }
        break;
    }

    if (argc < path_i + 1) {
        fprintf(stderr, "Usage: write [-offset=N] [-a] <path>\n");
        return 0;
    }

    int sock  = g_client->sock_fd;
    uint32_t flags = append_mode ? FLAG_APPEND : FLAG_NONE;

    /* Invia intestazione write */
    PayloadWrite pw;
    memset(&pw, 0, sizeof(pw));
    safe_strncpy(pw.path, argv[path_i], sizeof(pw.path));
    pw.offset = offset;
    if (net_send_msg(sock, (uint32_t)CMD_WRITE, flags,
                     &pw, sizeof(pw)) < 0) {
        fprintf(stderr, "write: send failed\n");
        return 0;
    }

    /* Attendi READY dal server */
    MsgHeader hdr;
    char resp[MAX_RESPONSE];
    if (net_recv_msg(sock, &hdr, resp, sizeof(resp)) < 0 ||
        hdr.cmd != (uint32_t)RES_OK) {
        size_t plen = hdr.payload_len < sizeof(resp) ? hdr.payload_len : sizeof(resp)-1;
        resp[plen] = '\0';
        fprintf(stderr, "write: server not ready: %s\n", resp);
        return 0;
    }

    printf("Enter text (terminate with a single '.' on a line):\n");

    char line[INPUT_BUFSIZE];
    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF su stdin (es. pipe, redirect): terminazione pulita */
            break;
        }

        char *trimmed = str_trim(line);
        if (strcmp(trimmed, ".") == 0) break;

        size_t len = strlen(line);
        PayloadDataChunk pkt;
        pkt.len = htonl((uint32_t)len);
        memcpy(pkt.data, line, len);
        uint32_t pkt_size = (uint32_t)(sizeof(uint32_t) + len);

        if (net_send_msg(sock, (uint32_t)CMD_WRITE_DATA,
                         FLAG_NONE, &pkt, pkt_size) < 0) {
            fprintf(stderr, "write: send chunk failed\n");
            return 0;
        }
    }

    net_send_msg(sock, (uint32_t)CMD_WRITE_END, FLAG_NONE, NULL, 0);
    handle_response();
    return 0;
}
