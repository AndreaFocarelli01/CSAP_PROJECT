#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "protocol.h"
#include "network.h"
#include "session.h"
#include "sync.h"
#include "filesystem.h"
#include "transfer.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <grp.h>
#include <arpa/inet.h>
#include <pwd.h>

/* ─────────────────────────────────────────────
 * Struttura globale del server
 * ───────────────────────────────────────────── */
typedef struct {
    char          vfs_root[MAX_PATH];
    char          ip[64];
    int           port;
    int           listen_fd;
    SessionTable *sess_tbl;
    LockTable    *lock_tbl;
    TransferTable*tr_tbl;
    gid_t         common_gid;    /* GID del gruppo SERVER_GROUP */
} ServerState;

static ServerState G;

/* ─────────────────────────────────────────────
 * Dichiarazioni forward
 * ───────────────────────────────────────────── */
static void worker_loop(int client_fd);
static int  cmd_create_user(int client_fd, const char *username,
                             const char *perm_str);
static int  cmd_login(int client_fd, const char *username,
                      int *sess_idx_out, Session **sess_out);
static void dispatch(int client_fd, Session *sess, int sess_idx,
                     MsgHeader *hdr, void *payload);
static void sigchld_handler(int sig);
static void sigterm_handler(int sig);
static void sigsegv_handler(int sig);
static int  ensure_vfs_root(const char *path);
static gid_t get_or_create_group(const char *groupname);

/* flag per terminazione server */
static volatile sig_atomic_t g_shutdown = 0;

/* ─────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Parsing argomenti: ./Server <root_dir> [<ip>] [<port>] */
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <root_directory> [<ip>] [<port>]\n"
                "       Defaults: ip=127.0.0.1 port=8080\n", argv[0]);
        return 1;
    }

    /* Salva euid root immediatamente, scende ai privilegi reali */
    drop_privileges();

    safe_strncpy(G.vfs_root, argv[1], sizeof(G.vfs_root));
    safe_strncpy(G.ip,  (argc >= 3) ? argv[2] : "127.0.0.1", sizeof(G.ip));
    G.port = (argc >= 4) ? atoi(argv[3]) : 8080;

    /* Crea/verifica root directory */
    if (ensure_vfs_root(G.vfs_root) < 0) return 1;

    /* Ottieni o crea gruppo comune */
    gain_privileges();
    G.common_gid = get_or_create_group(SERVER_GROUP);
    drop_privileges();
    if (G.common_gid == (gid_t)-1) {
        /* Fallback: usa il GID corrente del processo */
        G.common_gid = getgid();
        log_info("Group '%s' not found, using GID=%d as fallback",
                 SERVER_GROUP, (int)G.common_gid);
    }

    /* Inizializza strutture IPC (padre crea, figli si collegano) */
    G.sess_tbl = session_table_init(1);
    if (G.sess_tbl == NULL) { log_error("session_table_init failed"); return 1; }

    G.lock_tbl = sync_table_init(1);
    if (G.lock_tbl == NULL) { log_error("sync_table_init failed"); return 1; }

    G.tr_tbl = transfer_table_init(1);
    if (G.tr_tbl == NULL) { log_error("transfer_table_init failed"); return 1; }

    /* Socket in ascolto */
    G.listen_fd = net_server_init(G.ip, G.port);
    if (G.listen_fd < 0) return 1;

    /* SIGCHLD: raccolta zombie senza bloccarsi */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_NOCLDSTOP;   /* no SA_RESTART: accept() deve tornare */
    sigaction(SIGCHLD, &sa_chld, NULL);

    /* SIGTERM / SIGINT: shutdown pulito */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* SIGSEGV: log e exit */
    struct sigaction sa_segv;
    memset(&sa_segv, 0, sizeof(sa_segv));
    sa_segv.sa_handler = sigsegv_handler;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = 0;
    sigaction(SIGSEGV, &sa_segv, NULL);

    log_info("Server started. VFS root: %s", G.vfs_root);
    log_info("Listening on %s:%d", G.ip, G.port);

    /* ── Loop principale di accept ── */
    log_info("Entering accept loop (pid=%d)", (int)getpid());
    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        int client_fd = net_accept(G.listen_fd, &client_addr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                log_debug("accept EINTR, g_shutdown=%d", g_shutdown);
                continue;
            }
            if (g_shutdown) {
                log_debug("accept: shutdown flag set");
                break;
            }
            perror("accept");
            continue;
        }

        log_info("New connection from %s:%d",
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            net_close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* ─── FIGLIO (worker) ─── */
            /* Chiudi il listen socket nel figlio */
            close(G.listen_fd);

            /* Ri-collega le strutture IPC senza ricrearle */
            session_table_detach(G.sess_tbl);
            sync_table_detach(G.lock_tbl);
            transfer_table_detach(G.tr_tbl);

            G.sess_tbl = session_table_init(0);
            G.lock_tbl = sync_table_init(0);
            G.tr_tbl   = transfer_table_init(0);

            if (!G.sess_tbl || !G.lock_tbl || !G.tr_tbl) {
                log_error("Worker: cannot attach IPC structures");
                _exit(1);
            }

            /* SIGUSR1: richiesta transfer in arrivo */
            struct sigaction sa_usr1;
            memset(&sa_usr1, 0, sizeof(sa_usr1));
            sa_usr1.sa_handler = transfer_sigusr1_handler;
            sigemptyset(&sa_usr1.sa_mask);
            sa_usr1.sa_flags = 0;
            sigaction(SIGUSR1, &sa_usr1, NULL);

            worker_loop(client_fd);
            net_close(client_fd);
            _exit(0);
        }

        /* ─── PADRE ─── */
        close(client_fd);  /* il fd è del figlio */
    }

    /* Shutdown: segnala ai worker e attendi terminazione ordinata.
     * Usiamo SIGTERM su ciascun worker (non kill(0) che includerebbe
     * anche il processo padre stesso e i bg-worker dei client). */
    log_info("Server shutting down...");

    /* Invia SIGTERM solo ai figli diretti (worker) tramite gruppo di processo.
     * Usiamo kill(-getpgrp(), SIGTERM) con cautela, o iteriamo sui session PID. */
    {
        /* Soluzione sicura: kill su process group dei soli worker.
         * I worker creati da fork() condividono il pgid del padre.
         * Per non segnalare se stessi usiamo SIGTERM ai figli tracciati. */
        extern SessionTable *G_sess_tbl_ptr(void);
        /* Segnala ai worker rimasti tramite la session table */
        if (G.sess_tbl) {
            for (int _i = 0; _i < MAX_SESSIONS; _i++) {
                if (G.sess_tbl->sessions[_i].valid) {
                    pid_t wpid = G.sess_tbl->sessions[_i].worker_pid;
                    if (wpid > 0) kill(wpid, SIGTERM);
                }
            }
        }
    }

    /* Attendi figli con timeout: 3 secondi */
    {
        int waited = 0;
        for (int _t = 0; _t < 30 && waited >= 0; _t++) {
            struct timespec ts = { 0, 100000000L }; /* 100ms */
            nanosleep(&ts, NULL);
            while ((waited = (int)waitpid(-1, NULL, WNOHANG)) > 0);
        }
        /* Forza termine se ancora vivi */
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }

    /* Distruggi IPC */
    sync_table_destroy(G.lock_tbl);
    transfer_table_destroy(G.tr_tbl);
    session_table_destroy();

    net_close(G.listen_fd);
    log_info("Server terminated.");
    return 0;
}

/* ─────────────────────────────────────────────
 * SIGCHLD handler: raccoglie zombie
 * ───────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    /* waitpid con WNOHANG in loop: raccoglie tutti i figli terminati */
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ─────────────────────────────────────────────
 * SIGTERM/SIGINT handler
 * ───────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void sigsegv_handler(int sig) {
    (void)sig;
    /* Scrivi su stderr senza usare printf (non async-signal-safe) */
    const char msg[] = "[FATAL] SIGSEGV in server process\n";
    write(2, msg, sizeof(msg)-1);
    _exit(2);
}

/* ─────────────────────────────────────────────
 * ensure_vfs_root: crea la root directory se non esiste
 * ───────────────────────────────────────────── */
static int ensure_vfs_root(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            log_error("VFS root '%s' exists but is not a directory", path);
            return -1;
        }
        return 0;
    }
    if (mkdir(path, 0755) < 0) {
        perror("ensure_vfs_root mkdir");
        return -1;
    }
    log_info("Created VFS root: %s", path);
    return 0;
}

/* ─────────────────────────────────────────────
 * get_or_create_group
 * ───────────────────────────────────────────── */
static gid_t get_or_create_group(const char *groupname) {
    struct group *grp = getgrnam(groupname);
    if (grp != NULL) return grp->gr_gid;

    /* Crea il gruppo tramite addgroup */
    pid_t pid = fork();
    if (pid < 0) { perror("fork addgroup"); return (gid_t)-1; }

    if (pid == 0) {
        /* Esegui: addgroup --system <groupname> */
        gain_privileges();
        char *args[] = { "addgroup", "--system",
                         (char *)groupname, NULL };
        execvp("addgroup", args);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_info("addgroup not available for '%s', using current GID",
                 groupname);
        return (gid_t)-1;   /* chiamante usa fallback */
    }

    grp = getgrnam(groupname);
    return grp ? grp->gr_gid : (gid_t)-1;
}

/* ─────────────────────────────────────────────
 * cmd_create_user
 *
 * 1. Riacquisisce root.
 * 2. Fork + exec di: sudo adduser --disabled-password <username>
 * 3. Aggiunge l'utente al gruppo comune.
 * 4. Crea la home directory nel VFS.
 * 5. Rilascia root.
 * 6. Registra utente nella SessionTable.
 * ───────────────────────────────────────────── */
static int cmd_create_user(int client_fd, const char *username,
                            const char *perm_str) {
    mode_t perm;
    if (parse_permissions(perm_str, &perm) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: invalid permissions (use octal, e.g. 0755)");
        return -1;
    }

    /* Sanity check username */
    for (int i = 0; username[i]; i++) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            net_send_response(client_fd, RES_ERROR,
                              "ERROR: invalid username characters");
            return -1;
        }
    }

    /* Controlla se utente già registrato nel VFS */
    if (session_find_user(G.sess_tbl, username) != NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: user already exists");
        return -1;
    }

    /* ── Fork + exec adduser ── */
    gain_privileges();

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork adduser");
        drop_privileges();
        net_send_response(client_fd, RES_ERROR, "ERROR: fork failed");
        return -1;
    }

    if (pid == 0) {
        /* Figlio: esegui adduser --disabled-password --gecos "" <username> */
        char *args[] = {
            "sudo", "adduser",
            "--disabled-password",
            "--gecos", "",
            (char *)username,
            NULL
        };
        execvp("sudo", args);
        perror("execvp sudo adduser");
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        drop_privileges();
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: adduser failed (user may already exist at OS level)");
        return -1;
    }

    /* Aggiunge al gruppo comune */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "sudo", "usermod", "-aG",
                         SERVER_GROUP, (char *)username, NULL };
        execvp("sudo", args);
        perror("execvp usermod");
        _exit(1);
    }
    waitpid(pid, &status, 0);
    /* Non fatale se fallisce, logghiamo e continuiamo */
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        log_error("usermod -aG failed for '%s'", username);

    drop_privileges();

    /* Leggi uid/gid dell'utente appena creato */
    struct passwd *pw = getpwnam(username);
    if (pw == NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot read new user info");
        return -1;
    }
    uid_t new_uid = pw->pw_uid;
    gid_t new_gid = G.common_gid;

    /* Crea home directory nel VFS */
    char home_path[MAX_PATH];
    int w = snprintf(home_path, sizeof(home_path),
                     "%s/%s", G.vfs_root, username);
    if (w < 0 || (size_t)w >= sizeof(home_path)) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: path too long");
        return -1;
    }

    if (mkdir(home_path, perm) < 0 && errno != EEXIST) {
        perror("cmd_create_user mkdir");
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot create home directory");
        return -1;
    }

    /* Imposta proprietario e permessi della home */
    gain_privileges();
    if (chown(home_path, new_uid, new_gid) < 0)
        perror("chown home");
    if (chmod(home_path, perm) < 0)
        perror("chmod home");
    drop_privileges();

    /* Registra nella SessionTable */
    /* Il path home nel VFS è relativo alla root: /<username> */
    char vfs_home[MAX_PATH];
    snprintf(vfs_home, sizeof(vfs_home), "%s", home_path);

    if (session_add_user(G.sess_tbl, username, vfs_home,
                         perm, new_uid, new_gid) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot register user in session table");
        return -1;
    }

    char msg[MAX_USERNAME + 64];
    snprintf(msg, sizeof(msg), "OK User '%s' created", username);
    net_send_response(client_fd, RES_OK, msg);
    log_info("User '%s' created (uid=%d)", username, new_uid);
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_login
 * ───────────────────────────────────────────── */
static int cmd_login(int client_fd, const char *username,
                     int *sess_idx_out, Session **sess_out) {
    const UserRecord *ur = session_find_user(G.sess_tbl, username);
    if (ur == NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: user not found");
        return -1;
    }

    int sess_idx;
    if (session_open(G.sess_tbl, username, getpid(), &sess_idx) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot open session");
        return -1;
    }

    *sess_idx_out = sess_idx;
    *sess_out     = &G.sess_tbl->sessions[sess_idx];

    /* Registra il worker per le notifiche transfer */
    transfer_worker_init(G.tr_tbl, username, client_fd);

    char msg[MAX_USERNAME + 32];
    snprintf(msg, sizeof(msg), "OK Logged in as '%s'", username);
    net_send_response(client_fd, RES_OK, msg);
    log_info("User '%s' logged in (pid=%d)", username, (int)getpid());

    /* Controlla transfer PENDING arrivati mentre era offline
     * (DOPO RES_OK login, così il client è pronto a ricevere notifiche) */
    transfer_check_pending(G.tr_tbl, username, client_fd);
    return 0;
}

/* ─────────────────────────────────────────────
 * dispatch: esegue il comando ricevuto
 * ───────────────────────────────────────────── */
static void dispatch(int client_fd, Session *sess, int sess_idx,
                     MsgHeader *hdr, void *payload) {
    FsContext ctx;
    ctx.vfs_root  = G.vfs_root;
    ctx.sess      = sess;
    ctx.sess_tbl  = G.sess_tbl;
    ctx.lock_tbl  = G.lock_tbl;
    ctx.client_fd = client_fd;

    CommandCode cmd = (CommandCode)hdr->cmd;

    /* Comandi che non richiedono login */
    if (cmd == CMD_CREATE_USER) {
        PayloadCreateUser *p = (PayloadCreateUser *)payload;
        p->username[MAX_USERNAME - 1]  = '\0';
        p->permissions[15] = '\0';
        cmd_create_user(client_fd, p->username, p->permissions);
        return;
    }

    if (cmd == CMD_PING) {
        net_send_response(client_fd, RES_OK, "PONG");
        return;
    }

    /* Tutti gli altri comandi richiedono sessione attiva */
    if (sess == NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: not logged in");
        return;
    }

    switch (cmd) {

    case CMD_CREATE: {
        PayloadCreate *p = (PayloadCreate *)payload;
        p->path[MAX_PATH - 1] = '\0';
        p->permissions[15] = '\0';
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
        p->path[MAX_PATH - 1] = '\0';
        p->permissions[15] = '\0';
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
        if (fs_cd(&ctx, p->path) == 0) {
            /* Aggiorna SHM */
            session_set_cwd(G.sess_tbl, sess_idx, sess->cwd);
        }
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
            /* Background: fork figlio, torna subito */
            session_bg_inc(G.sess_tbl, sess_idx);
            pid_t bg = fork();
            if (bg == 0) {
                /* figlio background */
                char sp_copy[MAX_PATH];
                safe_strncpy(sp_copy, p->server_path, MAX_PATH);
                char cp_copy[MAX_PATH];
                safe_strncpy(cp_copy, p->client_path, MAX_PATH);
                fs_upload(&ctx, sp_copy);
                /* Notifica completamento */
                char done_msg[MAX_PATH * 2 + 64];
                snprintf(done_msg, sizeof(done_msg),
                         "[Background] Command: upload %s %s concluded.",
                         sp_copy, cp_copy);
                net_send_response(client_fd, RES_BG_DONE, done_msg);
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
                char sp_copy[MAX_PATH];
                safe_strncpy(sp_copy, p->server_path, MAX_PATH);
                char cp_copy[MAX_PATH];
                safe_strncpy(cp_copy, p->client_path, MAX_PATH);
                fs_download(&ctx, sp_copy);
                char done_msg[MAX_PATH * 2 + 64];
                snprintf(done_msg, sizeof(done_msg),
                         "[Background] Command: download %s %s concluded.",
                         sp_copy, cp_copy);
                net_send_response(client_fd, RES_BG_DONE, done_msg);
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
        p->file[MAX_PATH - 1]       = '\0';
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
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: unknown command");
        break;
    }
}

/* ─────────────────────────────────────────────
 * worker_loop
 * Loop principale del processo figlio.
 * Gestisce UN client per tutta la sua vita.
 * ───────────────────────────────────────────── */
static void worker_loop(int client_fd) {
    Session *sess      = NULL;
    int      sess_idx  = -1;
    int      logged_in = 0;

    /*
     * Figli background (upload/download -b): installa SA_NOCLDWAIT
     * così il kernel raccoglie automaticamente i figli del worker
     * al loro termine, senza bisogno di waitpid() esplicito.
     * Questo evita zombie per i processi background.
     */
    {
        struct sigaction sa_nocld;
        memset(&sa_nocld, 0, sizeof(sa_nocld));
        sa_nocld.sa_handler = SIG_DFL;
        sa_nocld.sa_flags   = SA_NOCLDWAIT;
        sigaction(SIGCHLD, &sa_nocld, NULL);
    }

    /* Buffer payload abbastanza grande per la struttura più grande */
    char payload[sizeof(PayloadUpload) + 16];

    int _maint_counter = 0;

    while (1) {
        /* Controlla segnale SIGUSR1 pendente */
        extern void transfer_process_pending_signal(void);
        transfer_process_pending_signal();

        /* Manutenzione periodica ogni 10 iterazioni:
         *  - purga sessioni orfane (worker crashati)
         *  - scade transfer PENDING troppo vecchi               */
        if (++_maint_counter >= 10) {
            _maint_counter = 0;
            session_cleanup_dead(G.sess_tbl);
            transfer_cleanup_expired(G.tr_tbl);
        }

        MsgHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        memset(payload, 0, sizeof(payload));

        int ret = net_recv_msg(client_fd, &hdr, payload, sizeof(payload));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout inattività (SO_RCVTIMEO scaduto) */
                log_info("Client idle timeout, closing (pid=%d)", (int)getpid());
            } else {
                log_info("Client disconnected (pid=%d)", (int)getpid());
            }
            break;
        }

        CommandCode cmd = (CommandCode)hdr.cmd;

        /* ── CMD_LOGIN gestito separatamente (modifica sess) ── */
        if (cmd == CMD_LOGIN) {
            /* Se già loggato, chiudi sessione precedente */
            if (logged_in && sess_idx >= 0) {
                session_close(G.sess_tbl, sess_idx);
                logged_in = 0;
                sess = NULL;
            }
            PayloadLogin *p = (PayloadLogin *)payload;
            p->username[MAX_USERNAME - 1] = '\0';
            if (cmd_login(client_fd, p->username,
                          &sess_idx, &sess) == 0) {
                logged_in = 1;
            }
            continue;
        }

        /* ── CMD_EXIT ── */
        if (cmd == CMD_EXIT) {
            int bg = logged_in
                   ? session_bg_count(G.sess_tbl, sess_idx)
                   : 0;

            if (bg > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "WAIT: %d background job(s) still running", bg);
                net_send_response(client_fd, RES_ERROR, msg);
                /* Non usciamo: il client deve aspettare e ritentare */
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

    /* Pulizia sessione */
    if (logged_in && sess_idx >= 0) {
        session_close(G.sess_tbl, sess_idx);
    }
}
