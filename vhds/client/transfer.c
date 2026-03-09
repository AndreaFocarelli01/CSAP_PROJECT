#define _POSIX_C_SOURCE 200809L

/*
 * client/transfer.c
 *
 * Gestione lato client di upload e download:
 *   - Lettura file locale e invio in chunk al server (upload)
 *   - Ricezione chunk dal server e scrittura file locale (download)
 *
 * Non usa strutture IPC (SHM/sem): quelle vivono solo nel server.
 * Il client transfer è puro I/O file + socket.
 */

#include "transfer.h"
#include "network.h"
#include "utils.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define UPLOAD_CHUNK    4096

/* ─────────────────────────────────────────────
 * Stub IPC (non usati lato client puro)
 * ───────────────────────────────────────────── */

TransferTable *transfer_table_init(int create) {
    (void)create;
    return NULL;
}

void transfer_table_detach(TransferTable *tbl) {
    (void)tbl;
}

void transfer_table_destroy(TransferTable *tbl) {
    (void)tbl;
}

/* ─────────────────────────────────────────────
 * Stub comandi server-side (non invocati dal client)
 * ───────────────────────────────────────────── */

int transfer_request(const char *r, Session *s, SessionTable *st,
                     LockTable *lt, TransferTable *tt, int fd,
                     const char *f, const char *du) {
    (void)r;(void)s;(void)st;(void)lt;(void)tt;(void)fd;(void)f;(void)du;
    return -1;
}

int transfer_accept(TransferTable *tt, SessionTable *st, int fd,
                    int id, const char *d, const char *u) {
    (void)tt;(void)st;(void)fd;(void)id;(void)d;(void)u;
    return -1;
}

int transfer_reject(TransferTable *tt, int fd, int id, const char *u) {
    (void)tt;(void)fd;(void)id;(void)u;
    return -1;
}

void transfer_sigusr1_handler(int sig)          { (void)sig; }
void transfer_process_pending_signal(void)      {}
void transfer_worker_init(TransferTable *t, const char *u, int fd)
                                                { (void)t;(void)u;(void)fd; }
void transfer_check_pending(TransferTable *t, const char *u, int fd)
                                                { (void)t;(void)u;(void)fd; }

/* ─────────────────────────────────────────────
 * client_upload_file
 *
 * Invia il file locale `local_path` al server
 * che lo salverà in `server_path`.
 *
 * Protocollo:
 *   1. Client → Server: CMD_UPLOAD (PayloadUpload)
 *   2. Server → Client: RES_OK "READY"
 *   3. Client → Server: N × CMD_WRITE_DATA (PayloadDataChunk)
 *   4. Client → Server: CMD_WRITE_END
 *   5. Server → Client: RES_OK "OK"
 *
 * Ritorna 0 su successo, -1 su errore.
 * ───────────────────────────────────────────── */
int client_upload_file(int sock_fd,
                       const char *local_path,
                       const char *server_path,
                       uint32_t flags) {
    /* Apri il file locale */
    int fd = open(local_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "upload: cannot open '%s': %s\n",
                local_path, strerror(errno));
        return -1;
    }

    /* Invia CMD_UPLOAD */
    PayloadUpload pup;
    memset(&pup, 0, sizeof(pup));
    safe_strncpy(pup.client_path, local_path,  sizeof(pup.client_path));
    safe_strncpy(pup.server_path, server_path, sizeof(pup.server_path));

    if (net_send_msg(sock_fd, (uint32_t)CMD_UPLOAD, flags,
                     &pup, sizeof(pup)) < 0) {
        close(fd);
        fprintf(stderr, "upload: send CMD_UPLOAD failed\n");
        return -1;
    }

    /* Attendi RES_OK "READY" dal server */
    MsgHeader hdr;
    char      resp[MAX_RESPONSE];
    if (net_recv_msg(sock_fd, &hdr, resp, sizeof(resp)) < 0 ||
        hdr.cmd != (uint32_t)RES_OK) {
        close(fd);
        fprintf(stderr, "upload: server not ready: %s\n",
                (char *)resp);
        return -1;
    }

    /* Invia file in chunk */
    char buf[UPLOAD_CHUNK];
    ssize_t nr;
    int     err = 0;

    while ((nr = read(fd, buf, sizeof(buf))) > 0) {
        PayloadDataChunk pkt;
        uint32_t chunk = (uint32_t)nr;
        pkt.len = htonl(chunk);
        memcpy(pkt.data, buf, chunk);
        uint32_t pkt_size = (uint32_t)(sizeof(uint32_t) + chunk);

        if (net_send_msg(sock_fd, (uint32_t)CMD_WRITE_DATA,
                         FLAG_NONE, &pkt, pkt_size) < 0) {
            err = 1;
            break;
        }
    }

    close(fd);

    if (err || nr < 0) {
        fprintf(stderr, "upload: read/send error\n");
        return -1;
    }

    /* Segnala fine stream */
    if (net_send_msg(sock_fd, (uint32_t)CMD_WRITE_END,
                     FLAG_NONE, NULL, 0) < 0) {
        fprintf(stderr, "upload: send CMD_WRITE_END failed\n");
        return -1;
    }

    /* Attendi conferma finale dal server */
    if (net_recv_msg(sock_fd, &hdr, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "upload: no final ACK from server\n");
        return -1;
    }

    if (hdr.cmd == (uint32_t)RES_OK) {
        printf("upload: OK\n");
        return 0;
    } else {
        fprintf(stderr, "upload: server error: %s\n", (char *)resp);
        return -1;
    }
}

/* ─────────────────────────────────────────────
 * client_download_file
 *
 * Riceve un file dal server e lo salva in `local_path`.
 *
 * Protocollo:
 *   1. Client → Server: CMD_DOWNLOAD (PayloadDownload)
 *   2. Server → Client: RES_OK "START"
 *   3. Server → Client: N × RES_DATA (PayloadDataChunk)
 *   4. Server → Client: RES_DATA_END
 *
 * Ritorna 0 su successo, -1 su errore.
 * ───────────────────────────────────────────── */
int client_download_file(int sock_fd,
                         const char *server_path,
                         const char *local_path,
                         uint32_t flags) {
    /* Invia CMD_DOWNLOAD */
    PayloadDownload pdl;
    memset(&pdl, 0, sizeof(pdl));
    safe_strncpy(pdl.server_path, server_path, sizeof(pdl.server_path));
    safe_strncpy(pdl.client_path, local_path,  sizeof(pdl.client_path));

    if (net_send_msg(sock_fd, (uint32_t)CMD_DOWNLOAD, flags,
                     &pdl, sizeof(pdl)) < 0) {
        fprintf(stderr, "download: send CMD_DOWNLOAD failed\n");
        return -1;
    }

    /* Prima risposta: RES_OK "START" oppure RES_ERROR */
    MsgHeader hdr;
    char      resp[MAX_RESPONSE];
    if (net_recv_msg(sock_fd, &hdr, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "download: no response from server\n");
        return -1;
    }

    if (hdr.cmd != (uint32_t)RES_OK) {
        fprintf(stderr, "download: server error: %s\n", (char *)resp);
        return -1;
    }

    /* Apri file locale in scrittura */
    int fd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "download: cannot create '%s': %s\n",
                local_path, strerror(errno));
        /* Drena lo stream per non lasciare il socket sporcato */
        return -1;
    }

    /* Ricevi stream di chunk */
    int err = 0;
    while (1) {
        char chunk_buf[sizeof(PayloadDataChunk) + 16];
        if (net_recv_msg(sock_fd, &hdr, chunk_buf, sizeof(chunk_buf)) < 0) {
            err = 1;
            break;
        }

        if (hdr.cmd == (uint32_t)RES_DATA_END) break;

        if (hdr.cmd != (uint32_t)RES_DATA) {
            fprintf(stderr, "download: unexpected response code %u\n",
                    hdr.cmd);
            err = 1;
            break;
        }

        if (hdr.payload_len < sizeof(uint32_t)) { err = 1; break; }

        uint32_t chunk_len;
        memcpy(&chunk_len, chunk_buf, sizeof(uint32_t));
        chunk_len = ntohl(chunk_len);

        if (chunk_len == 0 ||
            chunk_len > hdr.payload_len - sizeof(uint32_t)) {
            err = 1;
            break;
        }

        if (write_all(fd, chunk_buf + sizeof(uint32_t),
                      chunk_len) != (ssize_t)chunk_len) {
            fprintf(stderr, "download: write to local file failed\n");
            err = 1;
            break;
        }
    }

    close(fd);

    if (err) {
        unlink(local_path);    /* rimuove file parziale */
        fprintf(stderr, "download: failed, partial file removed\n");
        return -1;
    }

    printf("download: OK '%s'\n", local_path);
    return 0;
}
