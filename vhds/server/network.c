#define _POSIX_C_SOURCE 200809L

/*
 * network.c — Rete TCP con framing messaggi
 *
 * MIGLIORAMENTI:
 *  1. SO_KEEPALIVE + TCP_NODELAY su ogni socket (latenza + rilevazione
 *     connessioni morte).
 *  2. net_recv_msg: se il payload supera il buffer, drena i byte
 *     in eccesso anziché abortire l'intera connessione.
 *  3. net_send_msg: usa writev() per inviare header+payload in un
 *     unico syscall (evita il Nagle delay, riduce le syscall).
 *  4. net_server_init: SO_REUSEPORT per bilanciamento tra worker
 *     (dove disponibile).
 *  5. Timeout di ricezione configurabile (SO_RCVTIMEO).
 *  6. Validazione payload_len con limite esplicito (MAX_MSG_PAYLOAD).
 */

#include "network.h"
#include "utils.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

/* Limite assoluto per payload: 64 MiB */
#define MAX_MSG_PAYLOAD     (64u * 1024u * 1024u)

/* Timeout di ricezione idle: 120 secondi */
#define RECV_TIMEOUT_SEC    120

/* ─────────────────────────────────────────────
 * Opzioni socket comuni
 * ───────────────────────────────────────────── */
static int set_socket_options(int fd) {
    int opt = 1;

    /* Riutilizzo indirizzo */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }

    /* Keepalive: rileva connessioni cadute */
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_KEEPALIVE");
        /* non fatale */
    }

    /* TCP_NODELAY: disabilita Nagle, riduce la latenza dei messaggi piccoli */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY");
        /* non fatale */
    }

    /* Tuning keepalive: inizia dopo 60s idle, 3 probe ogni 10s */
#ifdef TCP_KEEPIDLE
    { int v = 60; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &v, sizeof(v)); }
    { int v = 3;  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &v, sizeof(v)); }
    { int v = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v)); }
#endif

    return 0;
}

/* ─────────────────────────────────────────────
 * SO_REUSEPORT (miglioramento scalabilità server multi-processo)
 * ───────────────────────────────────────────── */
static void try_set_reuseport(int fd) {
#ifdef SO_REUSEPORT
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    /* non fatale se non supportato */
#else
    (void)fd;
#endif
}

/* ─────────────────────────────────────────────
 * Timeout di ricezione
 * ───────────────────────────────────────────── */
static void set_recv_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* non fatale */
}

/* ─────────────────────────────────────────────
 * Server
 * ───────────────────────────────────────────── */
int net_set_reuseaddr(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }
    return 0;
}

int net_server_init(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    if (set_socket_options(fd) < 0) { close(fd); return -1; }
    try_set_reuseport(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (ip == NULL || strcmp(ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        log_error("Invalid IP address: %s", ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Backlog 64: gestisce burst di connessioni più grandi */
    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    log_info("Server listening on %s:%d", ip ? ip : "0.0.0.0", port);
    return fd;
}

int net_accept(int listen_fd, struct sockaddr_in *client_addr) {
    socklen_t addrlen = sizeof(struct sockaddr_in);
    int fd = accept(listen_fd, (struct sockaddr *)client_addr, &addrlen);
    if (fd < 0) return -1;

    /* Applica opzioni al socket client accettato */
    set_socket_options(fd);
    set_recv_timeout(fd, RECV_TIMEOUT_SEC);
    return fd;
}

/* ─────────────────────────────────────────────
 * Client
 * ───────────────────────────────────────────── */
int net_client_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    set_socket_options(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(ip);
        if (!he) {
            log_error("Cannot resolve host: %s", ip);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    log_info("Connected to %s:%d", ip, port);
    return fd;
}

/* ─────────────────────────────────────────────
 * Messaggi framed
 *
 * net_send_msg usa writev() per inviare header e payload
 * in un unico syscall — riduce overhead e latenza.
 * ───────────────────────────────────────────── */
int net_send_msg(int fd, uint32_t cmd, uint32_t flags,
                 const void *payload, uint32_t payload_len) {
    MsgHeader hdr;
    hdr.magic       = htonl(MSG_MAGIC);
    hdr.cmd         = htonl(cmd);
    hdr.payload_len = htonl(payload_len);
    hdr.flags       = htonl(flags);

    if (payload_len == 0 || payload == NULL) {
        /* Solo header */
        if (write_all(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            log_debug("net_send_msg: write header failed");
            return -1;
        }
        return 0;
    }

    /* Header + payload in un singolo writev */
    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len  = payload_len;

    size_t total = sizeof(hdr) + payload_len;
    size_t sent  = 0;
    size_t iov_idx = 0;

    while (sent < total) {
        ssize_t w = writev(fd, iov + iov_idx, (int)(2 - iov_idx));
        if (w < 0) {
            if (errno == EINTR) continue;
            log_debug("net_send_msg: writev failed: %s", strerror(errno));
            return -1;
        }
        sent += (size_t)w;

        /* Aggiorna iov per write parziale */
        size_t done = (size_t)w;
        for (size_t i = iov_idx; i < 2 && done > 0; i++) {
            if (done >= iov[i].iov_len) {
                done -= iov[i].iov_len;
                iov[i].iov_len = 0;
                iov_idx = i + 1;
            } else {
                iov[i].iov_base = (char *)iov[i].iov_base + done;
                iov[i].iov_len -= done;
                done = 0;
            }
        }
    }
    return 0;
}

/*
 * net_recv_msg: legge header e payload.
 * Se payload_len > buf_size, drena i byte in eccesso e restituisce
 * errore: il protocollo rimane sincronizzato (non chiudiamo la connessione
 * per un singolo messaggio troppo grande).
 */
int net_recv_msg(int fd, MsgHeader *hdr,
                 void *payload_buf, uint32_t buf_size) {
    if (read_all(fd, hdr, sizeof(MsgHeader)) != (ssize_t)sizeof(MsgHeader))
        return -1;

    hdr->magic       = ntohl(hdr->magic);
    hdr->cmd         = ntohl(hdr->cmd);
    hdr->payload_len = ntohl(hdr->payload_len);
    hdr->flags       = ntohl(hdr->flags);

    if (hdr->magic != MSG_MAGIC) {
        log_error("net_recv_msg: invalid magic 0x%08X (expected 0x%08X)",
                  hdr->magic, MSG_MAGIC);
        return -1;
    }

    if (hdr->payload_len == 0) return 0;

    /* Limite assoluto di sicurezza */
    if (hdr->payload_len > MAX_MSG_PAYLOAD) {
        log_error("net_recv_msg: payload too large %u (max %u)",
                  hdr->payload_len, MAX_MSG_PAYLOAD);
        return -1;
    }

    if (hdr->payload_len <= buf_size) {
        /* Caso normale: tutto nel buffer */
        if (read_all(fd, payload_buf, hdr->payload_len)
                != (ssize_t)hdr->payload_len) {
            log_debug("net_recv_msg: short read on payload");
            return -1;
        }
        return 0;
    }

    /* Payload più grande del buffer: leggi quello che entra, drena il resto */
    log_error("net_recv_msg: payload %u > buf %u, draining excess",
              hdr->payload_len, buf_size);

    if (read_all(fd, payload_buf, buf_size) != (ssize_t)buf_size)
        return -1;

    /* Drena i byte in eccesso */
    uint32_t excess = hdr->payload_len - buf_size;
    char     drain[256];
    while (excess > 0) {
        uint32_t n = (excess < sizeof(drain)) ? excess : (uint32_t)sizeof(drain);
        if (read_all(fd, drain, n) != (ssize_t)n) return -1;
        excess -= n;
    }

    /* Payload troncato: segnala con payload_len aggiornato */
    hdr->payload_len = buf_size;
    return -1;   /* errore: il chiamante deve gestirlo */
}

/* ─────────────────────────────────────────────
 * Risposte e stream
 * ───────────────────────────────────────────── */
int net_send_response(int fd, ResponseCode code, const char *msg) {
    char     buf[MAX_RESPONSE];
    uint32_t len = 0;

    if (msg != NULL) {
        safe_strncpy(buf, msg, sizeof(buf));
        len = (uint32_t)strlen(buf) + 1;
    }
    return net_send_msg(fd, (uint32_t)code, FLAG_NONE,
                        len > 0 ? buf : NULL, len);
}

int net_send_data_stream(int fd, const void *data, uint32_t total_len) {
    const char *ptr  = (const char *)data;
    uint32_t    sent = 0;

    while (sent < total_len) {
        uint32_t chunk = total_len - sent;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;

        PayloadDataChunk pkt;
        pkt.len = htonl(chunk);
        memcpy(pkt.data, ptr + sent, chunk);

        if (net_send_msg(fd, (uint32_t)RES_DATA, FLAG_NONE,
                         &pkt, sizeof(uint32_t) + chunk) < 0)
            return -1;
        sent += chunk;
    }

    return net_send_msg(fd, (uint32_t)RES_DATA_END, FLAG_NONE, NULL, 0);
}

int net_recv_data_stream(int fd, chunk_cb on_chunk, void *userdata) {
    MsgHeader hdr;
    char      payload[sizeof(PayloadDataChunk) + 16];

    while (1) {
        if (net_recv_msg(fd, &hdr, payload, sizeof(payload)) < 0)
            return -1;

        if (hdr.cmd == (uint32_t)RES_DATA_END) break;

        if (hdr.cmd != (uint32_t)RES_DATA) {
            log_error("net_recv_data_stream: unexpected cmd %u", hdr.cmd);
            return -1;
        }

        if (hdr.payload_len < sizeof(uint32_t)) return -1;
        uint32_t chunk_len;
        memcpy(&chunk_len, payload, sizeof(uint32_t));
        chunk_len = ntohl(chunk_len);
        if (chunk_len > hdr.payload_len - (uint32_t)sizeof(uint32_t))
            return -1;

        if (on_chunk &&
            on_chunk(payload + sizeof(uint32_t), chunk_len, userdata) < 0)
            return -1;
    }
    return 0;
}

void net_close(int fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
