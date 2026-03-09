#define _POSIX_C_SOURCE 200809L

#include "server.h"

/* ─────────────────────────────────────────────
 * main
 *
 * Entry point del server VHDS.
 * Tutta la logica è distribuita nei moduli:
 *   server_init.c     — setup, accept loop, shutdown
 *   server_signals.c  — gestori di segnale
 *   server_user.c     — create_user, login
 *   server_dispatch.c — dispatch comandi, worker loop
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (server_init(argc, argv) < 0)
        return 1;

    server_accept_loop();
    server_shutdown();
    return 0;
}
