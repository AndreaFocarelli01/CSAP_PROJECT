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
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>

/* ─────────────────────────────────────────────
 * cmd_create_user
 *
 * 1. Valida username e permessi.
 * 2. Verifica che non esista già nel VFS.
 * 3. Fork + exec "sudo adduser --disabled-password".
 * 4. Fork + exec "sudo usermod -aG <group>" (non fatale).
 * 5. Crea la home directory nel VFS con chown/chmod.
 * 6. Registra l'utente nella SessionTable.
 * ───────────────────────────────────────────── */
int cmd_create_user(int client_fd, const char *username,
                    const char *perm_str) {
    mode_t perm;
    if (parse_permissions(perm_str, &perm) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: invalid permissions (use octal, e.g. 0755)");
        return -1;
    }

    /* Sanity check: solo lettere minuscole, cifre, '_', '-' */
    for (int i = 0; username[i]; i++) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            net_send_response(client_fd, RES_ERROR,
                              "ERROR: invalid username characters");
            return -1;
        }
    }

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

    /* ── Fork + exec usermod (aggiunge al gruppo comune) ── */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "sudo", "usermod", "-aG",
                         SERVER_GROUP, (char *)username, NULL };
        execvp("sudo", args);
        perror("execvp usermod");
        _exit(1);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        log_error("usermod -aG failed for '%s'", username);

    drop_privileges();

    /* ── Leggi uid/gid del nuovo utente ── */
    struct passwd *pw = getpwnam(username);
    if (pw == NULL) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot read new user info");
        return -1;
    }
    uid_t new_uid = pw->pw_uid;
    gid_t new_gid = G.common_gid;

    /* ── Crea home directory nel VFS ── */
    char home_path[MAX_PATH];
    int w = snprintf(home_path, sizeof(home_path),
                     "%s/%s", G.vfs_root, username);
    if (w < 0 || (size_t)w >= sizeof(home_path)) {
        net_send_response(client_fd, RES_ERROR, "ERROR: path too long");
        return -1;
    }

    if (mkdir(home_path, perm) < 0 && errno != EEXIST) {
        perror("cmd_create_user mkdir");
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot create home directory");
        return -1;
    }

    gain_privileges();
    if (chown(home_path, new_uid, new_gid) < 0) perror("chown home");
    if (chmod(home_path, perm)             < 0) perror("chmod home");
    drop_privileges();

    /* ── Registra nella SessionTable ── */
    if (session_add_user(G.sess_tbl, username, home_path,
                         perm, new_uid, new_gid) < 0) {
        net_send_response(client_fd, RES_ERROR,
                          "ERROR: cannot register user in session table");
        return -1;
    }

    char msg[MAX_USERNAME + 64];
    snprintf(msg, sizeof(msg), "OK User '%s' created", username);
    net_send_response(client_fd, RES_OK, msg);
    log_info("User '%s' created (uid=%d)", username, (int)new_uid);
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_login
 *
 * Cerca l'utente nella SessionTable, apre la
 * sessione, registra il worker per le notifiche
 * transfer, e controlla eventuali transfer pending.
 * ───────────────────────────────────────────── */
int cmd_login(int client_fd, const char *username,
              int *sess_idx_out, Session **sess_out) {
    const UserRecord *ur = session_find_user(G.sess_tbl, username);
    if (ur == NULL) {
        net_send_response(client_fd, RES_ERROR, "ERROR: user not found");
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

    transfer_worker_init(G.tr_tbl, username, client_fd);

    char msg[MAX_USERNAME + 32];
    snprintf(msg, sizeof(msg), "OK Logged in as '%s'", username);
    net_send_response(client_fd, RES_OK, msg);
    log_info("User '%s' logged in (pid=%d)", username, (int)getpid());

    /* Notifiche transfer PENDING arrivate mentre era offline
     * (dopo RES_OK così il client è già pronto a riceverle) */
    transfer_check_pending(G.tr_tbl, username, client_fd);
    return 0;
}
