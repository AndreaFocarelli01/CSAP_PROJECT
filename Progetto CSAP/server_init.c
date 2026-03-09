#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "server.h"
#include "network.h"
#include "filesystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <grp.h>
#include <pwd.h>

/* ─────────────────────────────────────────────
 * Definizione delle variabili globali
 * (dichiarate extern in server.h)
 * ───────────────────────────────────────────── */
ServerState              G;
volatile sig_atomic_t    g_shutdown = 0;

/* ─────────────────────────────────────────────
 * ensure_vfs_root
 * Crea la directory radice del VFS se non esiste.
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
 * Cerca il gruppo SERVER_GROUP; lo crea tramite
 * addgroup se non esiste. Restituisce (gid_t)-1
 * su fallimento (il chiamante usa il GID corrente).
 * ───────────────────────────────────────────── */
static gid_t get_or_create_group(const char *groupname) {
    struct group *grp = getgrnam(groupname);
    if (grp != NULL) return grp->gr_gid;

    pid_t pid = fork();
    if (pid < 0) { perror("fork addgroup"); return (gid_t)-1; }

    if (pid == 0) {
        gain_privileges();
        char *args[] = { "addgroup", "--system", (char *)groupname, NULL };
        execvp("addgroup", args);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_info("addgroup not available for '%s', using current GID", groupname);
        return (gid_t)-1;
    }

    grp = getgrnam(groupname);
    return grp ? grp->gr_gid : (gid_t)-1;
}

/* ─────────────────────────────────────────────
 * server_init
 * Parsing argomenti, drop dei privilegi, IPC,
 * socket, segnali. Restituisce 0 su successo.
 * ───────────────────────────────────────────── */
int server_init(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <root_directory> [<ip>] [<port>]\n"
                "       Defaults: ip=127.0.0.1 port=8080\n", argv[0]);
        return -1;
    }

    drop_privileges();

    safe_strncpy(G.vfs_root, argv[1], sizeof(G.vfs_root));
    safe_strncpy(G.ip, (argc >= 3) ? argv[2] : "127.0.0.1", sizeof(G.ip));
    G.port = (argc >= 4) ? atoi(argv[3]) : 8080;

    if (ensure_vfs_root(G.vfs_root) < 0) return -1;

    gain_privileges();
    G.common_gid = get_or_create_group(SERVER_GROUP);
    drop_privileges();
    if (G.common_gid == (gid_t)-1) {
        G.common_gid = getgid();
        log_info("Group '%s' not found, using GID=%d as fallback",
                 SERVER_GROUP, (int)G.common_gid);
    }

    G.sess_tbl = session_table_init(1);
    if (!G.sess_tbl) { log_error("session_table_init failed"); return -1; }

    G.lock_tbl = sync_table_init(1);
    if (!G.lock_tbl) { log_error("sync_table_init failed"); return -1; }

    G.tr_tbl = transfer_table_init(1);
    if (!G.tr_tbl) { log_error("transfer_table_init failed"); return -1; }

    G.listen_fd = net_server_init(G.ip, G.port);
    if (G.listen_fd < 0) return -1;

    signals_init_parent();

    log_info("Server started. VFS root: %s", G.vfs_root);
    log_info("Listening on %s:%d", G.ip, G.port);
    return 0;
}

/* ─────────────────────────────────────────────
 * server_accept_loop
 * Loop principale: accept + fork per ogni client.
 * ───────────────────────────────────────────── */
void server_accept_loop(void) {
    log_info("Entering accept loop (pid=%d)", (int)getpid());

    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        int client_fd = net_accept(G.listen_fd, &client_addr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (g_shutdown)    break;
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
            close(G.listen_fd);

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

            signals_init_worker();
            worker_loop(client_fd);
            net_close(client_fd);
            _exit(0);
        }

        /* ─── PADRE ─── */
        close(client_fd);
    }
}

/* ─────────────────────────────────────────────
 * server_shutdown
 * Invia SIGTERM ai worker attivi, attende la loro
 * terminazione (max 3 s), distrugge le IPC.
 * ───────────────────────────────────────────── */
void server_shutdown(void) {
    log_info("Server shutting down...");

    if (G.sess_tbl) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (G.sess_tbl->sessions[i].valid) {
                pid_t wpid = G.sess_tbl->sessions[i].worker_pid;
                if (wpid > 0) kill(wpid, SIGTERM);
            }
        }
    }

    /* Attesa con timeout di 3 secondi (30 × 100 ms) */
    int waited = 0;
    for (int t = 0; t < 30 && waited >= 0; t++) {
        struct timespec ts = { 0, 100000000L };
        nanosleep(&ts, NULL);
        while ((waited = (int)waitpid(-1, NULL, WNOHANG)) > 0);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0);

    sync_table_destroy(G.lock_tbl);
    transfer_table_destroy(G.tr_tbl);
    session_table_destroy();

    net_close(G.listen_fd);
    log_info("Server terminated.");
}
