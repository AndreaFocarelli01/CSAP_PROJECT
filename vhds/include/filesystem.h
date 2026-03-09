#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <sys/types.h>
#include "session.h"
#include "sync.h"
#include "network.h"

/* ─────────────────────────────────────────────
 * Contesto di una operazione FS lato server
 * ───────────────────────────────────────────── */
typedef struct {
    const char   *vfs_root;     /* root assoluta del VFS sul disco    */
    Session      *sess;         /* sessione dell'utente che opera     */
    SessionTable *sess_tbl;     /* tabella sessioni (per permessi)    */
    LockTable    *lock_tbl;     /* tabella lock lettori/scrittori     */
    int           client_fd;    /* socket verso il client             */
    int           sess_idx;     /* indice sessione nella tabella      */
} FsContext;

/* ─────────────────────────────────────────────
 * Comandi FS
 * ───────────────────────────────────────────── */

/*
 * Crea un file vuoto (o una directory con is_dir=1).
 * `path`  : path relativo o assoluto (nel VFS).
 * `perm`  : permessi ottali.
 */
int fs_create(FsContext *ctx, const char *path, mode_t perm, int is_dir);

/*
 * Modifica i permessi di un file/directory.
 */
int fs_chmod(FsContext *ctx, const char *path, mode_t perm);

/*
 * Sposta/rinomina src → dst.
 */
int fs_move(FsContext *ctx, const char *src, const char *dst);

/*
 * Cambia la directory di lavoro corrente della sessione.
 * Aggiorna sess->cwd.
 */
int fs_cd(FsContext *ctx, const char *path);

/*
 * Elenca il contenuto di una directory.
 * Invia i dati direttamente al client tramite ctx->client_fd.
 * Rispetta le regole di visibilità per GROUP.
 */
int fs_list(FsContext *ctx, const char *path);

/*
 * Elimina un file (o directory vuota).
 */
int fs_delete(FsContext *ctx, const char *path);

/*
 * Legge un file e lo invia al client come stream RES_DATA.
 * `offset` = -1 → legge dall'inizio.
 */
int fs_read(FsContext *ctx, const char *path, int offset, int max_bytes);

/*
 * Riceve dati dal client (stream CMD_WRITE_DATA) e li scrive nel file.
 * `offset` = -1 → scrive dall'inizio (troncando).
 * Se il file non esiste, lo crea con permessi 0700.
 */
int fs_write(FsContext *ctx, const char *path, int offset, int append_mode);

/*
 * Riceve un file dal client (upload).
 * `server_path`: destinazione nel VFS.
 */
int fs_upload(FsContext *ctx, const char *server_path);

/*
 * Invia un file al client (download).
 * `server_path`: sorgente nel VFS.
 */
int fs_download(FsContext *ctx, const char *server_path);

/* ─────────────────────────────────────────────
 * Helper interni (usati anche da transfer.c)
 * ───────────────────────────────────────────── */

/*
 * Copia file da `src_abs` a `dst_abs` (path assoluti nel filesystem reale).
 * Non acquisisce lock (il chiamante deve averli già presi).
 */
int fs_copy_raw(const char *src_abs, const char *dst_abs);

/*
 * Verifica che l'utente abbia il permesso `check` (es. S_IRUSR)
 * sul file/dir a `abs_path`.
 * Tiene conto di uid/gid della sessione vs. owner del file.
 * Ritorna 1 se consentito, 0 se negato.
 */
int fs_check_permission(FsContext *ctx, const char *abs_path, int check);

/*
 * Converte un path VFS (relativo alla sessione) in path assoluto sul disco.
 * Ritorna 0 su successo, -1 se esce dalla root.
 */
int fs_resolve(FsContext *ctx, const char *vfs_path,
               char *abs_out, size_t abs_size);

#endif /* FILESYSTEM_H */
