#ifndef NETWORK_H
#define NETWORK_H

#include <sys/types.h>
#include <netinet/in.h>
#include "protocol.h"

/* ─────────────────────────────────────────────
 * Server
 * ───────────────────────────────────────────── */

/*
 * Crea socket TCP, bind su ip:port, listen.
 * Ritorna il fd del socket in ascolto, -1 su errore.
 */
int  net_server_init(const char *ip, int port);

/*
 * Accetta una connessione sul fd `listen_fd`.
 * Popola `client_addr`. Ritorna fd del client, -1 su errore.
 */
int  net_accept(int listen_fd, struct sockaddr_in *client_addr);

/* ─────────────────────────────────────────────
 * Client
 * ───────────────────────────────────────────── */

/*
 * Connette a ip:port.
 * Ritorna fd del socket connesso, -1 su errore.
 */
int  net_client_connect(const char *ip, int port);

/* ─────────────────────────────────────────────
 * Invio / Ricezione messaggi (framed)
 * ───────────────────────────────────────────── */

/*
 * Invia un messaggio completo: header + payload.
 * `payload` può essere NULL se payload_len == 0.
 * Ritorna 0 su successo, -1 su errore.
 */
int  net_send_msg(int fd, uint32_t cmd, uint32_t flags,
                  const void *payload, uint32_t payload_len);

/*
 * Riceve un messaggio: legge prima l'header, poi il payload.
 * `payload_buf` deve essere almeno `buf_size` byte.
 * Popola `hdr` con l'header ricevuto.
 * Ritorna 0 su successo, -1 su errore/connessione chiusa.
 */
int  net_recv_msg(int fd, MsgHeader *hdr,
                  void *payload_buf, uint32_t buf_size);

/*
 * Invia risposta semplice (RES_OK o RES_ERROR) con messaggio testuale.
 */
int  net_send_response(int fd, ResponseCode code, const char *msg);

/*
 * Invia un chunk di dati raw (RES_DATA) seguito da RES_DATA_END.
 * Usato da read/download.
 */
int  net_send_data_stream(int fd, const void *data, uint32_t len);

/*
 * Riceve stream di RES_DATA chunks fino a RES_DATA_END.
 * Chiama `on_chunk(data, len, userdata)` per ogni chunk ricevuto.
 * Ritorna 0 su successo, -1 su errore.
 */
typedef int (*chunk_cb)(const void *data, uint32_t len, void *userdata);
int  net_recv_data_stream(int fd, chunk_cb on_chunk, void *userdata);

/* ─────────────────────────────────────────────
 * Utilità socket
 * ───────────────────────────────────────────── */

/* Imposta SO_REUSEADDR */
int  net_set_reuseaddr(int fd);

/* Chiude il socket */
void net_close(int fd);

#endif /* NETWORK_H */
