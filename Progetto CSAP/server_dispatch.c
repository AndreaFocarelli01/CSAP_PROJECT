#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "server.h"
#include "network.h"
#include "filesystem.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────
 * dispatch
 *
 * Smista il comando ricevuto al gestore appropriato.
 * I comandi CMD_CREATE_USER e CMD_PING non richiedono
 * sessione; tutti gli altri vengono rifiutati se
 * sess == NULL (utente non autenticato).
 * ───────────────────────────────────────────── */
void dispatch(int client_fd, Session *sess, int sess_idx,
              MsgHeader *hdr, void *payload) {
    FsContext ctx;
    ctx.vfs_root  = G.vfs_root;
    ctx.sess      = sess;
    ctx.sess_tbl  = G.sess_tbl;
    ctx.lock_tbl  = G.lock_tbl;
    ctx.client_fd = client_fd;

    CommandCode cmd = (CommandCode)hdr->cmd;

    /* ── Comandi che non richiedono login ── */
    if (cmd == CMD_CREATE_USER) {
        PayloadCreateUser *p = (PayloadCreateUser *)payload;
        p->username[MAX_USERNAME - 1] = '\0';
        p->permissions[15]            = '\0';
        cmd_create_user(client_fd, p->username, p->permissions);
        return;
    }

    if (cmd == CMD_PING) {
        net_send_response(client_fd, RES_OK, "PONG");
        return;
    }

    /* ── Tutti gli altri richiedono sessione attiva ── */
    if (sess == NULL) {
        net_send_response(client_fd, RES_ERROR, "ERROR: not logged in");
        return;
    }

    switch (cmd) {

    case CMD_CREATE: {
        PayloadCreate *p = (PayloadCreate *)payload;
        p->path[MAX_PATH - 1]  = '\0';
        p->permissions[15]     = '\0';
        mode_t perm;
        if (parse_permissions(p->permissions, &perm) < 0) {
            net_send_response(client_fd, RES_ERROR,
                              "ERROR: invalid permissions");
            break;
        }
        int is_dir = (hdr->flags & FLAG_DIR) ? 1 : 0;
        fs_create(&ctx, p->path, perm, is_dir);
        break;
    }

    case CMD_CHMOD: {
        PayloadChmod *p = (PayloadChmod *)payload;
        p->path[MAX_PATH - 1]  = '\0';
        p->permissions[15]     = '\0';
        mode_t perm;
        if (parse_permissions(p->permissions, &perm) < 0) {
            net_send_response(client_fd, RES_ERROR,
                              "ERROR: invalid permissions");
            break;
        }
        fs_chmod(&ctx, p->path, perm);
        break;
    }

    case CMD_MOVE: {
        PayloadMove *p = (PayloadMove *)payload;
        p->src[MAX_PATH - 1] = '\0';
        p->dst[MAX_PATH - 1] = '\0';
        fs_move(&ctx, p->src, p->dst);
        break;
    }

    case CMD_CD: {
        PayloadPath *p = (PayloadPath *)payload;
        p->path[MAX_PATH - 1] = '\0';
        if (fs_cd(&ctx, p->path) == 0)
            session_set_cwd(G.sess_tbl, sess_idx, sess->cwd);
        break;
    }

    case CMD_LIST: {
        PayloadPath *p = (PayloadPath *)payload;
        p->path[MAX_PATH - 1] = '\0';
        const char *list_path = (p->path[0] == '\0') ? "." : p->path;
        fs_list(&ctx, list_path);
        break;
    }

    case CMD_DELETE: {
        PayloadPath *p = (PayloadPath *)payload;
        p->path[MAX_PATH - 1] = '\0';
        fs_delete(&ctx, p->path);
        break;
    }

    case CMD_READ: {
        PayloadRead *p = (PayloadRead *)payload;
        p->path[MAX_PATH - 1] = '\0';
        fs_read(&ctx, p->path, p->offset, p->max_bytes);
        break;
    }

    case CMD_WRITE: {
        PayloadWrite *p = (PayloadWrite *)payload;
        p->path[MAX_PATH - 1] = '\0';
        int append = (hdr->flags & FLAG_APPEND) != 0;
        fs_write(&ctx, p->path, p->offset, append);
        break;
    }

    case CMD_UPLOAD: {
        PayloadUpload *p = (PayloadUpload *)payload;
        p->server_path[MAX_PATH - 1] = '\0';
        p->client_path[MAX_PATH - 1] = '\0';

        if (hdr->flags & FLAG_BG) {
            session_bg_inc(G.sess_tbl, sess_idx);
            pid_t bg = fork();
            if (bg == 0) {
                char sp[MAX_PATH], cp[MAX_PATH];
                safe_strncpy(sp, p->server_path, MAX_PATH);
                safe_strncpy(cp, p->client_path, MAX_PATH);
                fs_upload(&ctx, sp);
                char done[MAX_PATH * 2 + 64];
                snprintf(done, sizeof(done),
                         "[Background] Command: upload %s %s concluded.", sp, cp);
                net_send_response(client_fd, RES_BG_DONE, done);
                session_bg_dec(G.sess_tbl, sess_idx);
                _exit(0);
            } else if (bg > 0) {
                net_send_response(client_fd, RES_OK,
                                  "OK Background upload started");
            } else {
                session_bg_dec(G.sess_tbl, sess_idx);
                net_send_response(client_fd, RES_ERROR,
                                  "ERROR: fork failed for background upload");
            }
        } else {
            fs_upload(&ctx, p->server_path);
        }
        break;
    }

    case CMD_DOWNLOAD: {
        PayloadDownload *p = (PayloadDownload *)payload;
        p->server_path[MAX_PATH - 1] = '\0';
        p->client_path[MAX_PATH - 1] = '\0';

        if (hdr->flags & FLAG_BG) {
            session_bg_inc(G.sess_tbl, sess_idx);
            pid_t bg = fork();
            if (bg == 0) {
                char sp[MAX_PATH], cp[MAX_PATH];
                safe_strncpy(sp, p->server_path, MAX_PATH);
                safe_strncpy(cp, p->client_path, MAX_PATH);
                fs_download(&ctx, sp);
                char done[MAX_PATH * 2 + 64];
                snprintf(done, sizeof(done),
                         "[Background] Command: download %s %s concluded.", sp, cp);
                net_send_response(client_fd, RES_BG_DONE, done);
                session_bg_dec(G.sess_tbl, sess_idx);
                _exit(0);
            } else if (bg > 0) {
                net_send_response(client_fd, RES_OK,
                                  "OK Background download started");
            } else {
                session_bg_dec(G.sess_tbl, sess_idx);
                net_send_response(client_fd, RES_ERROR,
                                  "ERROR: fork failed for background download");
            }
        } else {
            fs_download(&ctx, p->server_path);
        }
        break;
    }

    case CMD_TRANSFER_REQ: {
        PayloadTransferReq *p = (PayloadTransferReq *)payload;
        p->file[MAX_PATH - 1]        = '\0';
        p->dest_user[MAX_USERNAME-1] = '\0';
        transfer_request(G.vfs_root, sess, G.sess_tbl,
                         G.lock_tbl, G.tr_tbl,
                         client_fd, p->file, p->dest_user);
        break;
    }

    case CMD_ACCEPT: {
        PayloadAccept *p = (PayloadAccept *)payload;
        p->directory[MAX_PATH - 1] = '\0';
        transfer_accept(G.tr_tbl, G.sess_tbl, client_fd,
                        p->transfer_id, p->directory,
                        sess->username);
        break;
    }

    case CMD_REJECT: {
        PayloadReject *p = (PayloadReject *)payload;
        transfer_reject(G.tr_tbl, client_fd,
                        p->transfer_id, sess->username);
        break;
    }

    default:
        net_send_response(client_fd, RES_ERROR, "ERROR: unknown command");
        break;
    }
}

/* ─────────────────────────────────────────────
 * worker_loop
 *
 * Loop principale del processo figlio.
 * Gestisce un singolo client per tutta la durata
 * della sua connessione:
 *   - legge messaggi dal socket
 *   - gestisce CMD_LOGIN e CMD_EXIT direttamente
 *   - delega tutto il resto a dispatch()
 *   - esegue manutenzione periodica ogni 10 cicli
 * ───────────────────────────────────────────── */
void worker_loop(int client_fd) {
    Session *sess     = NULL;
    int      sess_idx = -1;
    int      logged_in = 0;
    int      maint_counter = 0;

    char payload[sizeof(PayloadUpload) + 16];

    while (1) {
        /* Controlla SIGUSR1 pendente (transfer request in arrivo) */
        transfer_process_pending_signal();

        /* Manutenzione periodica: purga sessioni orfane e transfer scaduti */
        if (++maint_counter >= 10) {
            maint_counter = 0;
            session_cleanup_dead(G.sess_tbl);
            transfer_cleanup_expired(G.tr_tbl);
        }

        memset(&payload, 0, sizeof(payload));
        MsgHeader hdr;
        memset(&hdr, 0, sizeof(hdr));

        int ret = net_recv_msg(client_fd, &hdr, payload, sizeof(payload));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                log_info("Client idle timeout, closing (pid=%d)", (int)getpid());
            else
                log_info("Client disconnected (pid=%d)", (int)getpid());
            break;
        }

        CommandCode cmd = (CommandCode)hdr.cmd;

        /* ── CMD_LOGIN ── */
        if (cmd == CMD_LOGIN) {
            if (logged_in && sess_idx >= 0) {
                session_close(G.sess_tbl, sess_idx);
                logged_in = 0;
                sess = NULL;
            }
            PayloadLogin *p = (PayloadLogin *)payload;
            p->username[MAX_USERNAME - 1] = '\0';
            if (cmd_login(client_fd, p->username, &sess_idx, &sess) == 0)
                logged_in = 1;
            continue;
        }

        /* ── CMD_EXIT ── */
        if (cmd == CMD_EXIT) {
            int bg = logged_in ? session_bg_count(G.sess_tbl, sess_idx) : 0;
            if (bg > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "WAIT: %d background job(s) still running", bg);
                net_send_response(client_fd, RES_ERROR, msg);
                continue;
            }
            net_send_response(client_fd, RES_OK, "BYE");
            break;
        }

        /* ── Tutti gli altri comandi ── */
        dispatch(client_fd,
                 logged_in ? sess : NULL,
                 sess_idx,
                 &hdr, payload);
    }

    if (logged_in && sess_idx >= 0)
        session_close(G.sess_tbl, sess_idx);
}
