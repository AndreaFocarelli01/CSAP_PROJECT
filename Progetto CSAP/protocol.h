#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* ─────────────────────────────────────────────
 * Dimensioni fisse dei campi del protocollo
 * ───────────────────────────────────────────── */
#define MAX_USERNAME    64
#define MAX_PATH        512
#define MAX_CMD_ARGS    8
#define MAX_ARG_LEN     512
#define MAX_PAYLOAD     4096
#define MAX_RESPONSE    4096

/* ─────────────────────────────────────────────
 * Codici Comando (Client → Server)
 * ───────────────────────────────────────────── */
typedef enum {
    CMD_CREATE_USER     = 1,
    CMD_LOGIN           = 2,
    CMD_CREATE          = 3,
    CMD_CHMOD           = 4,
    CMD_MOVE            = 5,
    CMD_CD              = 6,
    CMD_LIST            = 7,
    CMD_DELETE          = 8,
    CMD_READ            = 9,
    CMD_WRITE           = 10,
    CMD_UPLOAD          = 11,
    CMD_DOWNLOAD        = 12,
    CMD_TRANSFER_REQ    = 13,
    CMD_ACCEPT          = 14,
    CMD_REJECT          = 15,
    CMD_EXIT            = 16,
    CMD_WRITE_DATA      = 17,   /* chunk dati per write/upload */
    CMD_WRITE_END       = 18,   /* fine stream write/upload    */
    CMD_PING            = 19    /* keepalive / sincronizzazione */
} CommandCode;

/* ─────────────────────────────────────────────
 * Codici Risposta (Server → Client)
 * ───────────────────────────────────────────── */
typedef enum {
    RES_OK              = 0,
    RES_ERROR           = 1,
    RES_DATA            = 2,    /* chunk dati in uscita        */
    RES_DATA_END        = 3,    /* fine stream dati            */
    RES_NOTIFY          = 4,    /* notifica asincrona P2P      */
    RES_TRANSFER_REQ    = 5,    /* richiesta transfer in arrivo*/
    RES_TRANSFER_DONE   = 6,
    RES_TRANSFER_REJECT = 7,
    RES_BG_DONE         = 8     /* job background completato   */
} ResponseCode;

/* ─────────────────────────────────────────────
 * Header di ogni messaggio sul socket
 * (segue payload di lunghezza `payload_len`)
 * ───────────────────────────────────────────── */
typedef struct {
    uint32_t magic;         /* 0xDEADBEEF - validazione frame  */
    uint32_t cmd;           /* CommandCode o ResponseCode      */
    uint32_t payload_len;   /* byte del payload che seguono    */
    uint32_t flags;         /* flag generico (es. FLAG_BG)     */
} MsgHeader;

#define MSG_MAGIC       0xDEADBEEF
#define FLAG_NONE       0x00
#define FLAG_BG         0x01    /* operazione in background    */
#define FLAG_DIR        0x02    /* target è directory          */
#define FLAG_BINARY     0x04    /* trasferimento binario       */
#define FLAG_APPEND     0x08    /* write in append mode        */

/* ─────────────────────────────────────────────
 * Payload strutturati (cast sul buffer raw)
 * ───────────────────────────────────────────── */

/* CMD_CREATE_USER */
typedef struct {
    char username[MAX_USERNAME];
    char permissions[16];           /* es. "0755" */
} PayloadCreateUser;

/* CMD_LOGIN */
typedef struct {
    char username[MAX_USERNAME];
} PayloadLogin;

/* CMD_CREATE */
typedef struct {
    char path[MAX_PATH];
    char permissions[16];
} PayloadCreate;

/* CMD_CHMOD */
typedef struct {
    char path[MAX_PATH];
    char permissions[16];
} PayloadChmod;

/* CMD_MOVE */
typedef struct {
    char src[MAX_PATH];
    char dst[MAX_PATH];
} PayloadMove;

/* CMD_CD / CMD_DELETE / CMD_LIST */
typedef struct {
    char path[MAX_PATH];
} PayloadPath;

/* CMD_READ */
typedef struct {
    char     path[MAX_PATH];
    int32_t  offset;        /* -1 = nessun offset        */
    int32_t  max_bytes;     /* 0 = nessun limite         */
} PayloadRead;

/* CMD_WRITE (primo messaggio, poi CMD_WRITE_DATA chunks) */
typedef struct {
    char     path[MAX_PATH];
    int32_t  offset;
} PayloadWrite;

/* CMD_UPLOAD */
typedef struct {
    char client_path[MAX_PATH];
    char server_path[MAX_PATH];
} PayloadUpload;

/* CMD_DOWNLOAD */
typedef struct {
    char server_path[MAX_PATH];
    char client_path[MAX_PATH];
} PayloadDownload;

/* CMD_TRANSFER_REQ */
typedef struct {
    char file[MAX_PATH];
    char dest_user[MAX_USERNAME];
} PayloadTransferReq;

/* CMD_ACCEPT */
typedef struct {
    char directory[MAX_PATH];
    int32_t transfer_id;
} PayloadAccept;

/* CMD_REJECT */
typedef struct {
    int32_t transfer_id;
} PayloadReject;

/* RES_TRANSFER_REQ (notifica al destinatario) */
typedef struct {
    int32_t  transfer_id;
    char     src_user[MAX_USERNAME];
    char     filename[MAX_PATH];
} PayloadTransferNotify;

/* Chunk dati generico (CMD_WRITE_DATA / RES_DATA) */
typedef struct {
    uint32_t len;
    char     data[MAX_PAYLOAD];
} PayloadDataChunk;

#endif /* PROTOCOL_H */
