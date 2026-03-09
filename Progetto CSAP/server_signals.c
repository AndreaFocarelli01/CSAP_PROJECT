#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "transfer.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────
 * sigchld_handler
 * Raccoglie tutti i figli terminati senza bloccarsi.
 * ───────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ─────────────────────────────────────────────
 * sigterm_handler
 * Imposta il flag di shutdown per uscire dal loop.
 * ───────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ─────────────────────────────────────────────
 * sigsegv_handler
 * Log minimo async-signal-safe e uscita immediata.
 * ───────────────────────────────────────────── */
static void sigsegv_handler(int sig) {
    (void)sig;
    const char msg[] = "[FATAL] SIGSEGV in server process\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(2);
}

/* ─────────────────────────────────────────────
 * signals_init_parent
 * Installa i gestori nel processo padre (accept loop).
 * ───────────────────────────────────────────── */
void signals_init_parent(void) {
    struct sigaction sa;

    /* SIGCHLD: raccolta zombie, senza SA_RESTART (accept deve tornare) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* SIGTERM / SIGINT: shutdown pulito */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* SIGSEGV: log e uscita immediata */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);

    /* SIGPIPE: ignorato — gestiamo l'errore su write/send */
    signal(SIGPIPE, SIG_IGN);
}

/* ─────────────────────────────────────────────
 * signals_init_worker
 * Installa i gestori nel processo figlio (worker).
 * Il worker non gestisce SIGCHLD nello stesso modo:
 * usa SA_NOCLDWAIT per i propri figli background.
 * ───────────────────────────────────────────── */
void signals_init_worker(void) {
    struct sigaction sa;

    /* SIGUSR1: notifica di transfer in arrivo */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = transfer_sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* SIGCHLD nel worker: raccoglie automaticamente i figli background
     * (upload/download -b) senza waitpid esplicito */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, NULL);

    /* SIGPIPE: ignorato */
    signal(SIGPIPE, SIG_IGN);
}
