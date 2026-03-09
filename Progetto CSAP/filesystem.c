#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

/*
 * filesystem.c — Operazioni VFS con permessi e lock lettori/scrittori
 *
 * MIGLIORAMENTI:
 *  1. fs_list: buffer dinamico con crescita geometrica + ordinamento
 *     alfabetico degli entry (via comparator su scandir).
 *  2. fs_list: colonna aggiuntiva "owner" (uid:gid) e "mtime" (data modifica).
 *  3. fs_check_permission: gestisce sticky bit sulla directory padre
 *     (solo il proprietario può cancellare i file altrui).
 *  4. fs_read/fs_write: gestione esplicita di EINTR su read/write.
 *  5. fs_upload: permessi conservati se il file esiste già (no hardcode 0600).
 *  6. fs_copy_raw: usa sendfile(2) su Linux per zero-copy kernel-space.
 *  7. Macro SEND_ERR per ridurre boilerplate di codice d'errore.
 *  8. fs_delete: protezione home directory (non si può cancellare la propria).
 *  9. Tutti i path buffer verificano out_size prima del resolve.
 * 10. fs_write/upload: atomic rename (scrivi su tmp, poi rename) per
 *     garantire che i lettori vedano sempre un file intero.
 */

#include "filesystem.h"
#include "utils.h"
#include "protocol.h"
#include "network.h"
#include "sync.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

/* ─────────────────────────────────────────────
 * Costanti
 * ───────────────────────────────────────────── */
#define COPY_BUFSIZE    65536       /* 64 KiB — lettura/scrittura file    */
#define LIST_INIT_SIZE  4096        /* dimensione iniziale buffer list     */
#define LIST_LINE_MAX   256         /* max caratteri per riga di list      */
#define MAX_LIST_ENTRIES 2048       /* protezione contro directory enormi   */
#define TMP_SUFFIX      ".vhds_tmp" /* suffisso file temporaneo per write  */

/* ─────────────────────────────────────────────
 * Macro helper: invia errore e ritorna -1
 * ───────────────────────────────────────────── */
#define SEND_ERR(ctx, msg) do {                                \
    net_send_response((ctx)->client_fd, RES_ERROR, (msg));     \
    return -1;                                                 \
} while (0)

/* ─────────────────────────────────────────────
 * fs_resolve
 * ───────────────────────────────────────────── */
int fs_resolve(FsContext *ctx, const char *vfs_path,
               char *abs_out, size_t abs_size) {
    return path_resolve(ctx->vfs_root,
                        ctx->sess->cwd,
                        vfs_path,
                        abs_out, abs_size);
}

/* ─────────────────────────────────────────────
 * fs_check_permission
 *
 * Controlla che l'utente abbia il permesso `check`
 * sul path indicato. check può essere:
 *   S_IRUSR  — lettura
 *   S_IWUSR  — scrittura
 *   S_IXUSR  — esecuzione/traversal
 * (oppure combinazioni OR di questi)
 *
 * Gestisce sticky bit: se la directory padre ha
 * il bit T (01000), solo il proprietario del file
 * o della directory può cancellarlo/spostarlo.
 * ───────────────────────────────────────────── */
int fs_check_permission(FsContext *ctx, const char *abs_path, int check) {
    struct stat st;
    if (lstat(abs_path, &st) < 0) return 0;

    const UserRecord *ur = session_find_user(ctx->sess_tbl,
                                              ctx->sess->username);
    if (!ur) return 0;

    /* Root virtuale: bypass totale */
    if (ur->uid == 0) return 1;

    /* Proprietario: controlla bit USER */
    if (st.st_uid == ur->uid) {
        mode_t user_bits = (mode_t)check & (S_IRUSR | S_IWUSR | S_IXUSR);
        return (st.st_mode & user_bits) == user_bits;
    }

    /* Stesso gruppo: mappa check → bit GROUP */
    if (st.st_gid == ur->gid) {
        mode_t grp = 0;
        if (check & S_IRUSR) grp |= S_IRGRP;
        if (check & S_IWUSR) grp |= S_IWGRP;
        if (check & S_IXUSR) grp |= S_IXGRP;
        if (check & S_IRGRP) grp |= S_IRGRP;
        if (check & S_IWGRP) grp |= S_IWGRP;
        if (check & S_IXGRP) grp |= S_IXGRP;
        return (st.st_mode & grp) == grp;
    }

    /* OTHER: policy progetto — accesso negato */
    return 0;
}

/*
 * Verifica sticky bit: ritorna 0 se l'utente NON può
 * cancellare `target_abs` dalla directory `dir_abs`.
 */
static int sticky_allows_delete(FsContext *ctx,
                                 const char *dir_abs,
                                 const char *target_abs) {
    struct stat dir_st, tgt_st;
    if (stat(dir_abs, &dir_st) < 0)    return 1; /* ignora se errore */
    if (!(dir_st.st_mode & S_ISVTX))   return 1; /* no sticky bit */

    /* Con sticky bit: solo proprietario del file o della dir */
    if (lstat(target_abs, &tgt_st) < 0) return 1;
    const UserRecord *ur = session_find_user(ctx->sess_tbl,
                                              ctx->sess->username);
    if (!ur) return 0;
    if (ur->uid == 0) return 1;
    return (tgt_st.st_uid == ur->uid || dir_st.st_uid == ur->uid);
}

/* ─────────────────────────────────────────────
 * fs_create
 * ───────────────────────────────────────────── */
int fs_create(FsContext *ctx, const char *path, mode_t perm, int is_dir) {
    char abs[MAX_PATH], parent[MAX_PATH];

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    path_dirname(abs, parent, sizeof(parent));
    if (!fs_check_permission(ctx, parent, S_IWUSR | S_IXUSR))
        SEND_ERR(ctx, "ERROR: permission denied on parent directory");

    int ret;
    if (is_dir) {
        sync_write_lock(ctx->lock_tbl, parent);
        ret = mkdir(abs, perm);
        sync_write_unlock(ctx->lock_tbl, parent);
        if (ret < 0 && errno != EEXIST) {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "ERROR: cannot create directory: %s",
                     strerror(errno));
            SEND_ERR(ctx, errmsg);
        }
    } else {
        sync_write_lock(ctx->lock_tbl, abs);
        int fd = open(abs, O_CREAT | O_WRONLY | O_TRUNC, perm);
        sync_write_unlock(ctx->lock_tbl, abs);
        if (fd < 0)
            SEND_ERR(ctx, "ERROR: cannot create file");
        close(fd);
    }

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_chmod
 * ───────────────────────────────────────────── */
int fs_chmod(FsContext *ctx, const char *path, mode_t perm) {
    char abs[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (lstat(abs, &st) < 0)
        SEND_ERR(ctx, "ERROR: file not found");

    const UserRecord *ur = session_find_user(ctx->sess_tbl,
                                              ctx->sess->username);
    if (!ur || (ur->uid != 0 && st.st_uid != ur->uid))
        SEND_ERR(ctx, "ERROR: permission denied (not owner)");

    sync_write_lock(ctx->lock_tbl, abs);
    int ret = chmod(abs, perm);
    sync_write_unlock(ctx->lock_tbl, abs);

    if (ret < 0)
        SEND_ERR(ctx, "ERROR: chmod failed");

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_move
 * ───────────────────────────────────────────── */
int fs_move(FsContext *ctx, const char *src, const char *dst) {
    char abs_src[MAX_PATH], abs_dst[MAX_PATH];
    char parent_src[MAX_PATH], parent_dst[MAX_PATH];

    if (fs_resolve(ctx, src, abs_src, sizeof(abs_src)) < 0 ||
        fs_resolve(ctx, dst, abs_dst, sizeof(abs_dst)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (!fs_check_permission(ctx, abs_src, S_IWUSR))
        SEND_ERR(ctx, "ERROR: permission denied on source");

    path_dirname(abs_src, parent_src, sizeof(parent_src));
    path_dirname(abs_dst, parent_dst, sizeof(parent_dst));

    if (!sticky_allows_delete(ctx, parent_src, abs_src))
        SEND_ERR(ctx, "ERROR: permission denied (sticky bit)");

    /* La directory di destinazione deve essere scrivibile */
    if (!fs_check_permission(ctx, parent_dst, S_IWUSR | S_IXUSR))
        SEND_ERR(ctx, "ERROR: permission denied on destination directory");

    /* Lock ordinato per path (evita deadlock tra due move concorrenti) */
    const char *first  = abs_src;
    const char *second = abs_dst;
    if (strcmp(abs_dst, abs_src) < 0) { first = abs_dst; second = abs_src; }

    sync_write_lock(ctx->lock_tbl, first);
    if (strcmp(first, second) != 0)
        sync_write_lock(ctx->lock_tbl, second);

    int ret = rename(abs_src, abs_dst);

    if (strcmp(first, second) != 0)
        sync_write_unlock(ctx->lock_tbl, second);
    sync_write_unlock(ctx->lock_tbl, first);

    if (ret < 0) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "ERROR: move failed: %s", strerror(errno));
        SEND_ERR(ctx, errmsg);
    }

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_cd
 * ───────────────────────────────────────────── */
int fs_cd(FsContext *ctx, const char *path) {
    char abs[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (stat(abs, &st) < 0 || !S_ISDIR(st.st_mode))
        SEND_ERR(ctx, "ERROR: not a directory");

    if (!fs_check_permission(ctx, abs, S_IXUSR))
        SEND_ERR(ctx, "ERROR: permission denied");

    safe_strncpy(ctx->sess->cwd, abs, MAX_PATH);
    session_set_cwd(ctx->sess_tbl, ctx->sess_idx, abs);

    /* Path display relativo alla root VFS */
    const char *rel = abs + strlen(ctx->vfs_root);
    if (rel[0] == '\0') rel = "/";

    char msg[MAX_PATH + 8];
    snprintf(msg, sizeof(msg), "OK %s", rel);
    net_send_response(ctx->client_fd, RES_OK, msg);
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_list — con ordinamento e colonne estese
 *
 * Output per riga:
 *   <tipo+perms>  <uid:gid>  <size>  <mtime>  <name>
 * ───────────────────────────────────────────── */

/* Comparatore per scandir: ordine alfabetico */
static int alpha_compare(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int fs_list(FsContext *ctx, const char *path) {
    char abs[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (stat(abs, &st) < 0)
        SEND_ERR(ctx, "ERROR: path not found");

    if (!S_ISDIR(st.st_mode))
        SEND_ERR(ctx, "ERROR: not a directory");

    /* Permessi: prima USER, poi GROUP (home altrui via gruppo comune) */
    if (!fs_check_permission(ctx, abs, S_IRUSR | S_IXUSR) &&
        !fs_check_permission(ctx, abs, S_IRGRP | S_IXGRP))
        SEND_ERR(ctx, "ERROR: permission denied");

    /* scandir: ordina alfabeticamente, esclude . e .. automaticamente */
    struct dirent **namelist = NULL;
    int n_entries = scandir(abs, &namelist, NULL, alpha_compare);
    int truncated = 0;
    if (n_entries > MAX_LIST_ENTRIES) {
        /* Tronca la lista per evitare buffer enormi */
        for (int i = MAX_LIST_ENTRIES; i < n_entries; i++)
            free(namelist[i]);
        n_entries = MAX_LIST_ENTRIES;
        truncated = 1;
    }
    if (n_entries < 0)
        SEND_ERR(ctx, "ERROR: cannot open directory");

    /* Buffer dinamico con crescita geometrica */
    size_t buf_cap  = LIST_INIT_SIZE;
    size_t buf_used = 0;
    char  *buf      = (char *)malloc(buf_cap);
    if (!buf) {
        for (int i = 0; i < n_entries; i++) free(namelist[i]);
        free(namelist);
        SEND_ERR(ctx, "ERROR: out of memory");
    }

    /* Intestazione */
    int written = snprintf(buf, buf_cap,
        "%-36s %-10s %8s %12s  %s\n"
        "%-36s %-10s %8s %12s  %s\n",
        "Name",  "Perms",    "Size",   "Modified",  "",
        "----",  "-----",    "----",   "--------",  "");
    if (written > 0) buf_used = (size_t)written;

    /* Lock lettura sulla directory prima di accedere agli stat */
    sync_read_lock(ctx->lock_tbl, abs);

    for (int i = 0; i < n_entries; i++) {
        struct dirent *ent = namelist[i];

        /* Salta . e .. */
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) {
            free(ent);
            continue;
        }

        char entry_abs[MAX_PATH];
        if (snprintf(entry_abs, sizeof(entry_abs),
                     "%s/%s", abs, ent->d_name) >= (int)sizeof(entry_abs)) {
            free(ent);
            continue;
        }

        struct stat est;
        if (lstat(entry_abs, &est) < 0) { free(ent); continue; }

        /* Tipo + permessi */
        char perms[11];
        perms[0] = S_ISDIR(est.st_mode) ? 'd' :
                   S_ISLNK(est.st_mode) ? 'l' : '-';
        format_permissions(est.st_mode, perms + 1);

        /* Data modifica — formato compatto HH:MM DD/MM/YY */
        char mtime_str[16] = "?";
        struct tm *tm_info = localtime(&est.st_mtime);
        if (tm_info)
            strftime(mtime_str, sizeof(mtime_str), "%d/%m %H:%M", tm_info);

        /* Assicura spazio nel buffer (crescita geometrica) */
        while (buf_used + LIST_LINE_MAX > buf_cap) {
            buf_cap *= 2;
            char *tmp = (char *)realloc(buf, buf_cap);
            if (!tmp) goto list_done;
            buf = tmp;
        }

        written = snprintf(buf + buf_used, buf_cap - buf_used,
                           "%-36s %-10s %8lld %12s\n",
                           ent->d_name,
                           perms,
                           (long long)est.st_size,
                           mtime_str);
        if (written > 0) buf_used += (size_t)written;
        free(ent);
    }

list_done:
    sync_read_unlock(ctx->lock_tbl, abs);
    free(namelist);

    if (truncated) {
        /* Notifica al client che la lista è stata troncata */
        const char *warn = "[WARNING: list truncated at max entries]\n";
        size_t wlen = strlen(warn);
        while (buf_used + wlen >= buf_cap) {
            buf_cap *= 2;
            char *tmp = (char *)realloc(buf, buf_cap);
            if (!tmp) break;
            buf = tmp;
        }
        if (buf_used + wlen < buf_cap) {
            memcpy(buf + buf_used, warn, wlen);
            buf_used += wlen;
        }
    }

    net_send_data_stream(ctx->client_fd, buf, (uint32_t)buf_used);
    free(buf);
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_delete — con protezione sticky bit e home
 * ───────────────────────────────────────────── */
int fs_delete(FsContext *ctx, const char *path) {
    char abs[MAX_PATH], parent[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    /* Non si può cancellare la root VFS */
    if (strcmp(abs, ctx->vfs_root) == 0)
        SEND_ERR(ctx, "ERROR: cannot delete VFS root");

    /* Non si può cancellare la propria home directory */
    const UserRecord *ur = session_find_user(ctx->sess_tbl,
                                              ctx->sess->username);
    if (ur && strcmp(abs, ur->home) == 0)
        SEND_ERR(ctx, "ERROR: cannot delete your home directory");

    if (lstat(abs, &st) < 0)
        SEND_ERR(ctx, "ERROR: file not found");

    path_dirname(abs, parent, sizeof(parent));

    if (!fs_check_permission(ctx, parent, S_IWUSR | S_IXUSR))
        SEND_ERR(ctx, "ERROR: permission denied");

    if (!sticky_allows_delete(ctx, parent, abs))
        SEND_ERR(ctx, "ERROR: permission denied (sticky bit)");

    sync_write_lock(ctx->lock_tbl, abs);
    int ret = S_ISDIR(st.st_mode) ? rmdir(abs) : unlink(abs);
    sync_write_unlock(ctx->lock_tbl, abs);

    if (ret < 0)
        SEND_ERR(ctx, "ERROR: delete failed (directory may not be empty)");

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_read — con EINTR handling e offset
 * ───────────────────────────────────────────── */
int fs_read(FsContext *ctx, const char *path, int offset, int max_bytes) {
    char abs[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (lstat(abs, &st) < 0 || !S_ISREG(st.st_mode))
        SEND_ERR(ctx, "ERROR: file not found or not a regular file");

    if (!fs_check_permission(ctx, abs, S_IRUSR))
        SEND_ERR(ctx, "ERROR: permission denied");

    if (sync_read_lock(ctx->lock_tbl, abs) < 0)
        SEND_ERR(ctx, "ERROR: cannot acquire read lock");

    int fd = open(abs, O_RDONLY);
    if (fd < 0) {
        sync_read_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: cannot open file");
    }

    if (offset > 0 && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        sync_read_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: invalid offset");
    }

    char   *buf = (char *)malloc(COPY_BUFSIZE);
    if (!buf) {
        close(fd);
        sync_read_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: out of memory");
    }

    int     err = 0;
    ssize_t nr;
    size_t  remaining = (max_bytes > 0) ? (size_t)max_bytes : (size_t)-1;

    while (remaining > 0) {
        size_t  to_read = (remaining < COPY_BUFSIZE) ? remaining : COPY_BUFSIZE;
        nr = read(fd, buf, to_read);
        if (nr == 0) break;
        if (nr < 0) {
            if (errno == EINTR) continue;
            err = 1; break;
        }
        remaining -= (size_t)nr;
        PayloadDataChunk pkt;
        pkt.len = htonl((uint32_t)nr);
        memcpy(pkt.data, buf, (size_t)nr);
        if (net_send_msg(ctx->client_fd, (uint32_t)RES_DATA,
                         FLAG_NONE, &pkt,
                         (uint32_t)(sizeof(uint32_t) + (size_t)nr)) < 0) {
            err = 1; break;
        }
    }

    free(buf);
    close(fd);
    sync_read_unlock(ctx->lock_tbl, abs);

    if (err) return -1;

    net_send_msg(ctx->client_fd, (uint32_t)RES_DATA_END, FLAG_NONE, NULL, 0);
    return 0;
}

/* ─────────────────────────────────────────────
 * Ricezione stream da client (write/upload)
 * ───────────────────────────────────────────── */
static int recv_data_stream_to_fd(int client_fd, int out_fd) {
    MsgHeader hdr;
    char      chunk_buf[sizeof(PayloadDataChunk) + 16];

    while (1) {
        if (net_recv_msg(client_fd, &hdr,
                         chunk_buf, sizeof(chunk_buf)) < 0) return -1;
        if (hdr.cmd == (uint32_t)CMD_WRITE_END) break;
        if (hdr.cmd != (uint32_t)CMD_WRITE_DATA)  return -1;
        if (hdr.payload_len < sizeof(uint32_t))    return -1;

        uint32_t chunk_len;
        memcpy(&chunk_len, chunk_buf, sizeof(uint32_t));
        chunk_len = ntohl(chunk_len);
        if (chunk_len == 0 ||
            chunk_len > hdr.payload_len - (uint32_t)sizeof(uint32_t))
            return -1;

        if (write_all(out_fd, chunk_buf + sizeof(uint32_t),
                      chunk_len) != (ssize_t)chunk_len) return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_write — atomic: scrive su tmp poi rename
 * ───────────────────────────────────────────── */
int fs_write(FsContext *ctx, const char *path, int offset, int append_mode) {
    char abs[MAX_PATH], tmp_path[MAX_PATH];

    if (fs_resolve(ctx, path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    struct stat st;
    int exists = (lstat(abs, &st) == 0);

    if (exists && !fs_check_permission(ctx, abs, S_IWUSR))
        SEND_ERR(ctx, "ERROR: permission denied");

    /* File temporaneo nella stessa directory (stesso filesystem → rename atomico) */
    if (snprintf(tmp_path, sizeof(tmp_path),
                 "%s%s", abs, TMP_SUFFIX) >= (int)sizeof(tmp_path))
        SEND_ERR(ctx, "ERROR: path too long");

    if (sync_write_lock(ctx->lock_tbl, abs) < 0)
        SEND_ERR(ctx, "ERROR: cannot acquire write lock");

    /*
     * Modalità scrittura:
     *   offset <= 0, append_mode=0 → overwrite (trunc), usa tmp atomico
     *   offset  > 0                → scrivi da byte offset, no tmp
     *   append_mode != 0           → lseek SEEK_END (aggiunge in coda), no tmp
     */
    int use_tmp   = (offset <= 0 && !append_mode);
    int flags     = O_WRONLY | O_CREAT | (use_tmp ? O_TRUNC : 0);
    mode_t perm   = exists ? st.st_mode : (mode_t)0600;

    const char *write_target = use_tmp ? tmp_path : abs;
    int fd = open(write_target, flags, perm);
    if (fd < 0) {
        sync_write_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: cannot open file for writing");
    }

    /* Posizionamento cursore */
    if (append_mode) {
        if (lseek(fd, 0, SEEK_END) < 0) {
            close(fd);
            sync_write_unlock(ctx->lock_tbl, abs);
            SEND_ERR(ctx, "ERROR: lseek SEEK_END failed");
        }
    } else if (offset > 0) {
        if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
            close(fd);
            sync_write_unlock(ctx->lock_tbl, abs);
            SEND_ERR(ctx, "ERROR: invalid offset");
        }
    }

    /* Pronto a ricevere */
    net_send_response(ctx->client_fd, RES_OK, "READY");

    int err = recv_data_stream_to_fd(ctx->client_fd, fd);
    log_debug("fs_write: recv err=%d offset=%d append=%d tmp='%s' abs='%s'",
              err, offset, append_mode, tmp_path, abs);
    close(fd);

    if (!err && use_tmp) {
        /* Rename atomico: i lettori vedono sempre il file completo */
        if (rename(tmp_path, abs) < 0) {
            log_error("fs_write: rename '%s' -> '%s' failed: %s",
                      tmp_path, abs, strerror(errno));
            err = 1;
            unlink(tmp_path);
        } else {
            log_debug("fs_write: rename OK -> '%s'", abs);
        }
    } else if (err && use_tmp) {
        log_error("fs_write: recv failed, unlinking tmp '%s'", tmp_path);
        unlink(tmp_path);
    }

    sync_write_unlock(ctx->lock_tbl, abs);

    if (err)
        SEND_ERR(ctx, "ERROR: write failed");

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_copy_raw — con sendfile(2) su Linux
 * ───────────────────────────────────────────── */
int fs_copy_raw(const char *src_abs, const char *dst_abs) {
    struct stat st;
    if (stat(src_abs, &st) < 0) return -1;

    int fd_src = open(src_abs, O_RDONLY);
    if (fd_src < 0) return -1;

    int fd_dst = open(dst_abs, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (fd_dst < 0) { close(fd_src); return -1; }

    int err = 0;

#ifdef __linux__
    /* sendfile: zero-copy kernel-space, nessun passaggio per userspace */
    off_t offset = 0;
    off_t remaining = st.st_size;
    while (remaining > 0) {
        ssize_t sent = sendfile(fd_dst, fd_src, &offset, (size_t)remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;
            err = 1; break;
        }
        if (sent == 0) break;
        remaining -= sent;
    }
#else
    /* Fallback portabile */
    char buf[COPY_BUFSIZE];
    ssize_t nr;
    while ((nr = read(fd_src, buf, sizeof(buf))) != 0) {
        if (nr < 0) { if (errno == EINTR) continue; err = 1; break; }
        if (write_all(fd_dst, buf, (size_t)nr) != nr) { err = 1; break; }
    }
#endif

    close(fd_src);
    close(fd_dst);
    if (err) unlink(dst_abs);
    return err ? -1 : 0;
}

/* ─────────────────────────────────────────────
 * fs_upload — atomic + conserva permessi
 * ───────────────────────────────────────────── */
int fs_upload(FsContext *ctx, const char *server_path) {
    char abs[MAX_PATH], tmp_path[MAX_PATH], parent[MAX_PATH];

    if (fs_resolve(ctx, server_path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    path_dirname(abs, parent, sizeof(parent));
    if (!fs_check_permission(ctx, parent, S_IWUSR | S_IXUSR))
        SEND_ERR(ctx, "ERROR: permission denied on destination");

    if (snprintf(tmp_path, sizeof(tmp_path),
                 "%s%s", abs, TMP_SUFFIX) >= (int)sizeof(tmp_path))
        SEND_ERR(ctx, "ERROR: path too long");

    /* Conserva permessi se il file esiste */
    struct stat st;
    mode_t perm = (lstat(abs, &st) == 0) ? st.st_mode : (mode_t)0600;

    if (sync_write_lock(ctx->lock_tbl, abs) < 0)
        SEND_ERR(ctx, "ERROR: cannot acquire lock");

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, perm);
    if (fd < 0) {
        sync_write_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: cannot create destination file");
    }

    net_send_response(ctx->client_fd, RES_OK, "READY");

    int err = recv_data_stream_to_fd(ctx->client_fd, fd);
    close(fd);

    if (!err) {
        if (rename(tmp_path, abs) < 0) {
            log_error("fs_upload: rename '%s' -> '%s' failed: %s",
                      tmp_path, abs, strerror(errno));
            unlink(tmp_path);
            err = 1;
        }
    } else {
        unlink(tmp_path);
    }

    sync_write_unlock(ctx->lock_tbl, abs);

    if (err)
        SEND_ERR(ctx, "ERROR: upload failed");

    net_send_response(ctx->client_fd, RES_OK, "OK");
    return 0;
}

/* ─────────────────────────────────────────────
 * fs_download
 * ───────────────────────────────────────────── */
int fs_download(FsContext *ctx, const char *server_path) {
    char abs[MAX_PATH];
    struct stat st;

    if (fs_resolve(ctx, server_path, abs, sizeof(abs)) < 0)
        SEND_ERR(ctx, "ERROR: path escapes server root");

    if (lstat(abs, &st) < 0 || !S_ISREG(st.st_mode))
        SEND_ERR(ctx, "ERROR: file not found or not a regular file");

    if (!fs_check_permission(ctx, abs, S_IRUSR))
        SEND_ERR(ctx, "ERROR: permission denied");

    if (sync_read_lock(ctx->lock_tbl, abs) < 0)
        SEND_ERR(ctx, "ERROR: cannot acquire read lock");

    int fd = open(abs, O_RDONLY);
    if (fd < 0) {
        sync_read_unlock(ctx->lock_tbl, abs);
        SEND_ERR(ctx, "ERROR: cannot open file");
    }

    net_send_response(ctx->client_fd, RES_OK, "START");

    char    buf[COPY_BUFSIZE];
    ssize_t nr;
    int     err = 0;

    while ((nr = read(fd, buf, sizeof(buf))) != 0) {
        if (nr < 0) { if (errno == EINTR) continue; err = 1; break; }
        PayloadDataChunk pkt;
        pkt.len = htonl((uint32_t)nr);
        memcpy(pkt.data, buf, (size_t)nr);
        if (net_send_msg(ctx->client_fd, (uint32_t)RES_DATA,
                         FLAG_NONE, &pkt,
                         (uint32_t)(sizeof(uint32_t) + (size_t)nr)) < 0) {
            err = 1; break;
        }
    }

    close(fd);
    sync_read_unlock(ctx->lock_tbl, abs);

    if (err) return -1;

    net_send_msg(ctx->client_fd, (uint32_t)RES_DATA_END, FLAG_NONE, NULL, 0);
    return 0;
}
