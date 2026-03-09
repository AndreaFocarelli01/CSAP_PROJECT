#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

/*
 * main_client.c — Entrypoint client VHDS
 *
 * Responsabilità di questo file:
 *   - Parsing argomenti, connessione al server
 *   - Installazione signal handler (SIGCHLD, SIGPIPE)
 *   - Dispatch tabella comandi
 *   - Loop readline: history, espansione !!, prompt
 *
 * Tutto il resto è nei moduli:
 *   history.c     — ring buffer history
 *   notify.c      — handle_response, drain_async_notifications
 *   cmd_auth.c    — login, create_user, exit, ping, help
 *   cmd_fs.c      — create, chmod, move, cd, list, delete, read, write
 *   cmd_transfer.c— upload, download, transfer_request, accept, reject
 */

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────
 * Stato globale (definizione — dichiarato extern in client.h)
 * ───────────────────────────────────────────── */
ClientState  g_state;
ClientState *g_client = &g_state;

/* ─────────────────────────────────────────────
 * SIGCHLD: raccoglie processi background
 * ───────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        if (g_client->bg_count > 0)
            g_client->bg_count--;
    }
}

/* ─────────────────────────────────────────────
 * Tabella dispatch comandi
 *
 * Ogni entry: { "nome", handler_fn }
 * process_line() cerca il nome e chiama handler(argc, argv).
 * handler ritorna:
 *   0  → continua loop
 *   1  → esci (confermato da exit)
 *  -1  → errore non fatale
 * ───────────────────────────────────────────── */
typedef int (*cmd_handler)(int argc, char **argv);

typedef struct {
    const char *name;
    cmd_handler  fn;
} CmdEntry;

static const CmdEntry dispatch_table[] = {
    /* Autenticazione e sessione */
    { "login",            cmd_login            },
    { "create_user",      cmd_create_user      },
    { "exit",             cmd_exit             },
    { "ping",             cmd_ping             },
    { "help",             cmd_help             },
    /* Filesystem */
    { "create",           cmd_create           },
    { "chmod",            cmd_chmod            },
    { "move",             cmd_move             },
    { "cd",               cmd_cd               },
    { "list",             cmd_list             },
    { "delete",           cmd_delete           },
    { "read",             cmd_read             },
    { "write",            cmd_write            },
    /* Trasferimento */
    { "upload",           cmd_upload           },
    { "download",         cmd_download         },
    { "transfer_request", cmd_transfer_request },
    { "accept",           cmd_accept           },
    { "reject",           cmd_reject           },
    /* Sentinella */
    { NULL, NULL }
};

/* ─────────────────────────────────────────────
 * print_prompt
 * ───────────────────────────────────────────── */
static void print_prompt(void) {
    if (g_client->logged_in) {
        const char *cwd = g_client->cwd[0] ? g_client->cwd : "/";
        if (g_client->bg_count > 0)
            printf("\033[1;36m[%s@vhds %s]\033[0m {bg:%d}$ ",
                   g_client->username, cwd, (int)g_client->bg_count);
        else
            printf("\033[1;36m[%s@vhds %s]\033[0m$ ",
                   g_client->username, cwd);
    } else {
        printf("\033[1;90m[guest@vhds]\033[0m$ ");
    }
    fflush(stdout);
}

/* ─────────────────────────────────────────────
 * process_line: dispatch su tabella
 * ───────────────────────────────────────────── */
static int process_line(char *line) {
    char *argv[MAX_CMD_ARGS];
    int   argc = str_tokenize(line, argv, MAX_CMD_ARGS);
    if (argc == 0) return 0;

    const char *name = argv[0];

    /* Comando speciale gestito nel loop: history */
    if (strcmp(name, "history") == 0) {
        history_print();
        return 0;
    }

    for (int i = 0; dispatch_table[i].name != NULL; i++) {
        if (strcmp(name, dispatch_table[i].name) == 0)
            return dispatch_table[i].fn(argc, argv);
    }

    fprintf(stderr, "Unknown command: '%s'  (type \033[1mhelp\033[0m)\n", name);
    return 0;
}

/* ─────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    const char *ip   = argv[1];
    int         port = atoi(argv[2]);

    /* Inizializza stato */
    memset(g_client, 0, sizeof(*g_client));
    snprintf(g_client->server_addr, sizeof(g_client->server_addr),
             "%s:%d", ip, port);
    safe_strncpy(g_client->cwd, "/", sizeof(g_client->cwd));

    /* Connessione */
    g_client->sock_fd = net_client_connect(ip, port);
    if (g_client->sock_fd < 0) return 1;

    /* Segnali */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Banner */
    printf("\033[1;32m╔══════════════════════════════════════╗\033[0m\n"
           "\033[1;32m║   VHDS — Virtual Home Directory Sys  ║\033[0m\n"
           "\033[1;32m╚══════════════════════════════════════╝\033[0m\n"
           "Connected to \033[1m%s\033[0m\n"
           "Type \033[1mhelp\033[0m for commands, "
           "\033[1mhistory\033[0m / \033[1m!!\033[0m to recall previous.\n\n",
           g_client->server_addr);

    /* ── Loop principale ── */
    char line[INPUT_BUFSIZE];

    while (1) {
        drain_async_notifications();
        print_prompt();

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (Ctrl-D o pipe chiusa) */
            if (g_client->bg_count > 0) {
                fprintf(stderr,
                        "\nWaiting for %d background job(s) to finish...\n",
                        (int)g_client->bg_count);
                while ((int)g_client->bg_count > 0)
                    pause();
            }
            printf("\n");
            break;
        }

        char *trimmed = str_trim(line);
        if (!trimmed[0]) continue;

        /* Espansione !!/!N */
        const char *expanded = history_expand(trimmed);
        if (expanded != NULL) {
            if (!expanded[0]) continue;   /* errore già stampato */
            trimmed = (char *)expanded;
        }

        history_add(trimmed);

        int ret = process_line(trimmed);
        if (ret == 1) break;   /* exit confermato */
    }

    history_save();
    net_close(g_client->sock_fd);
    return 0;
}
