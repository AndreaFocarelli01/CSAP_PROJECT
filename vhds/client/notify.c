#define _POSIX_C_SOURCE 200809L

/*
 * notify.c — Ricezione risposte e notifiche asincrone
 *
 * Esporta:
 *   drain_async_notifications()  — polling non-bloccante su select()
 *   handle_response()            — legge UN messaggio sincrono
 *   recv_data_stream_to_stdout() — legge stream RES_DATA → stdout
 */

#include "client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <arpa/inet.h>

/* ─────────────────────────────────────────────
 * Stampa notifica transfer in arrivo
 * ───────────────────────────────────────────── */
static void print_transfer_req(const void *payload) {
    const PayloadTransferNotify *ntf =
        (const PayloadTransferNotify *)payload;

    /* Copia difensiva per terminazione sicura */
    char fname[MAX_PATH];
    char uname[MAX_USERNAME];
    safe_strncpy(fname, ntf->filename, sizeof(fname));
    safe_strncpy(uname, ntf->src_user, sizeof(uname));

    printf("\n\033[1;33m[TRANSFER REQUEST]\033[0m "
           "ID=%d  from \033[1m%s\033[0m: \033[1m%s\033[0m\n"
           "  accept <directory> %d   OR   reject %d\n",
           ntf->transfer_id, uname, fname,
           ntf->transfer_id, ntf->transfer_id);
    fflush(stdout);
}

/* ─────────────────────────────────────────────
 * Gestisci UN messaggio asincrono noto
 * Ritorna 1 se era una notifica asincrona, 0 altrimenti.
 * ───────────────────────────────────────────── */
static int handle_async(const MsgHeader *hdr, const void *payload) {
    switch ((ResponseCode)hdr->cmd) {

    case RES_TRANSFER_REQ:
        print_transfer_req(payload);
        return 1;

    case RES_BG_DONE:
        if (hdr->payload_len > 0)
            printf("\n\033[1;34m[BG]\033[0m %s\n", (const char *)payload);
        else
            printf("\n\033[1;34m[BG]\033[0m Background job completed.\n");
        fflush(stdout);
        return 1;

    case RES_TRANSFER_DONE:
        printf("\n\033[1;32m[TRANSFER]\033[0m Completed.\n");
        fflush(stdout);
        return 1;

    case RES_TRANSFER_REJECT:
        printf("\n\033[1;31m[TRANSFER]\033[0m Rejected by remote user.\n");
        fflush(stdout);
        return 1;

    default:
        return 0;
    }
}

/* ─────────────────────────────────────────────
 * drain_async_notifications
 *
 * Usa select() con timeout=0 per svuotare il socket
 * di eventuali notifiche asincrone pendenti.
 * Non blocca mai: ritorna appena non ci sono più dati.
 * ───────────────────────────────────────────── */
void drain_async_notifications(void) {
    int sock = g_client->sock_fd;

    while (1) {
        struct timeval tv = { 0, 0 };
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sock, &rset);

        int sel = select(sock + 1, &rset, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) break;
        if (!FD_ISSET(sock, &rset)) break;

        MsgHeader hdr;
        char      payload[sizeof(PayloadTransferNotify) + MAX_RESPONSE + 16];

        if (net_recv_msg(sock, &hdr, payload, sizeof(payload)) < 0) break;

        if (!handle_async(&hdr, payload)) {
            /* Messaggio non-asincrono arrivato fuori turno: ignora con log */
            log_debug("drain: unexpected msg code=%u len=%u",
                      hdr.cmd, hdr.payload_len);
        }
    }
}

/* ─────────────────────────────────────────────
 * handle_response
 *
 * Legge UN messaggio sincrono.
 * Se arrivano notifiche asincrone prima della risposta attesa,
 * le gestisce e continua ad attendere.
 *
 * Ritorna:
 *   0  → RES_OK (stampa payload)
 *  -1  → RES_ERROR o errore rete (stampa su stderr)
 *   N  → altro ResponseCode (es. RES_DATA=2): il chiamante
 *         gestisce lo stream da solo
 * ───────────────────────────────────────────── */
int handle_response(void) {
    int   sock = g_client->sock_fd;
    MsgHeader hdr;
    char  payload[MAX_RESPONSE + 16];

    /* Loop per assorbire notifiche asincrone interlacciate */
    while (1) {
        if (net_recv_msg(sock, &hdr, payload, sizeof(payload)) < 0) {
            fprintf(stderr, "Connection lost.\n");
            return -1;
        }
        if (!handle_async(&hdr, payload)) break;
    }

    /* Assicura terminazione stringa payload */
    size_t plen = (hdr.payload_len < (uint32_t)sizeof(payload))
                   ? hdr.payload_len : sizeof(payload) - 1;
    payload[plen] = '\0';

    if (hdr.cmd == (uint32_t)RES_OK) {
        printf("%s\n", payload);
        return 0;
    }

    if (hdr.cmd == (uint32_t)RES_ERROR) {
        fprintf(stderr, "%s\n", payload);
        return -1;
    }

    if (hdr.cmd == (uint32_t)RES_TRANSFER_DONE) {
        printf("\033[1;32m[TRANSFER]\033[0m %s\n",
               plen > 0 ? payload : "Transfer completed.");
        return 0;
    }

    if (hdr.cmd == (uint32_t)RES_TRANSFER_REJECT) {
        fprintf(stderr, "\033[1;31m[TRANSFER]\033[0m %s\n",
                plen > 0 ? payload : "Transfer rejected.");
        return -1;
    }

    /* RES_DATA o altro: segnala codice al chiamante */
    return (int)hdr.cmd;
}

/* ─────────────────────────────────────────────
 * recv_data_stream_to_stdout
 *
 * Legge lo stream RES_DATA → RES_DATA_END e scrive su stdout.
 * Deve essere chiamata DOPO aver già letto il primo chunk
 * (o DOPO aver ricevuto RES_DATA_END) — vedi cmd_list / cmd_read
 * che gestiscono il primo messaggio separatamente per poter
 * controllare RES_ERROR prima di iniziare lo stream.
 *
 * Ritorna 0 su successo, -1 su errore.
 * ───────────────────────────────────────────── */
int recv_data_stream_to_stdout(void) {
    int   sock = g_client->sock_fd;
    MsgHeader hdr;
    char  chunk_buf[sizeof(PayloadDataChunk) + 16];

    while (1) {
        if (net_recv_msg(sock, &hdr, chunk_buf, sizeof(chunk_buf)) < 0)
            return -1;
        if (hdr.cmd == (uint32_t)RES_DATA_END) break;
        if (hdr.cmd != (uint32_t)RES_DATA)     return -1;
        if (hdr.payload_len < (uint32_t)sizeof(uint32_t)) return -1;

        uint32_t clen;
        memcpy(&clen, chunk_buf, sizeof(uint32_t));
        clen = ntohl(clen);

        uint32_t avail = hdr.payload_len - (uint32_t)sizeof(uint32_t);
        if (clen > avail) clen = avail;
        if (clen > 0)
            write_all(STDOUT_FILENO, chunk_buf + sizeof(uint32_t), clen);
    }
    fflush(stdout);
    return 0;
}
