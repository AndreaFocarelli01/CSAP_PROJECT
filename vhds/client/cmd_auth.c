#define _POSIX_C_SOURCE 200809L

/*
 * cmd_auth.c — Comandi di autenticazione e sessione
 *
 *   login, create_user, exit, ping, help
 */

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────
 * cmd_ping
 * ───────────────────────────────────────────── */
int cmd_ping(int argc, char **argv) {
    (void)argc; (void)argv;
    int sock = g_client->sock_fd;

    net_send_msg(sock, (uint32_t)CMD_PING, FLAG_NONE, NULL, 0);

    MsgHeader hdr;
    char      resp[MAX_RESPONSE];
    if (net_recv_msg(sock, &hdr, resp, sizeof(resp)) == 0) {
        resp[hdr.payload_len < sizeof(resp) ? hdr.payload_len : sizeof(resp)-1] = '\0';
        printf("%s\n", hdr.payload_len > 0 ? resp : "PONG");
    } else {
        fprintf(stderr, "ERROR: no response to ping\n");
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_create_user
 * ───────────────────────────────────────────── */
int cmd_create_user(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: create_user <username> <permissions>\n");
        return 0;
    }
    PayloadCreateUser p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.username,    argv[1], sizeof(p.username));
    safe_strncpy(p.permissions, argv[2], sizeof(p.permissions));
    net_send_msg(g_client->sock_fd, (uint32_t)CMD_CREATE_USER,
                 FLAG_NONE, &p, sizeof(p));
    handle_response();
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_login
 * ───────────────────────────────────────────── */
int cmd_login(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: login <username>\n");
        return 0;
    }

    int sock = g_client->sock_fd;
    PayloadLogin p;
    memset(&p, 0, sizeof(p));
    safe_strncpy(p.username, argv[1], sizeof(p.username));
    net_send_msg(sock, (uint32_t)CMD_LOGIN, FLAG_NONE, &p, sizeof(p));

    MsgHeader hdr;
    char resp[MAX_RESPONSE];
    if (net_recv_msg(sock, &hdr, resp, sizeof(resp)) != 0) {
        fprintf(stderr, "login: connection error\n");
        return 0;
    }

    size_t plen = hdr.payload_len < sizeof(resp) ? hdr.payload_len : sizeof(resp)-1;
    resp[plen] = '\0';

    if (hdr.cmd == (uint32_t)RES_OK) {
        g_client->logged_in = 1;
        safe_strncpy(g_client->username, argv[1], sizeof(g_client->username));
        safe_strncpy(g_client->cwd, "/", sizeof(g_client->cwd));
        printf("%s\n", resp);
    } else {
        fprintf(stderr, "%s\n", resp);
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * cmd_exit
 *
 * Ritorna 1 se il server conferma l'uscita (loop si chiude).
 * Ritorna 0 se bloccato da background jobs o dal server.
 * ───────────────────────────────────────────── */
int cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;

    if (g_client->bg_count > 0) {
        fprintf(stderr,
                "Cannot exit: %d background job(s) still running.\n"
                "Wait for them to complete first.\n",
                (int)g_client->bg_count);
        return 0;
    }

    int sock = g_client->sock_fd;
    net_send_msg(sock, (uint32_t)CMD_EXIT, FLAG_NONE, NULL, 0);

    MsgHeader hdr;
    char resp[MAX_RESPONSE];
    if (net_recv_msg(sock, &hdr, resp, sizeof(resp)) == 0) {
        size_t plen = hdr.payload_len < sizeof(resp)
                       ? hdr.payload_len : sizeof(resp)-1;
        resp[plen] = '\0';
        if (hdr.cmd == (uint32_t)RES_ERROR) {
            fprintf(stderr, "%s\n", resp);
            return 0;
        }
        printf("%s\n", resp);
    }
    return 1;   /* segnala al loop di terminare */
}

/* ─────────────────────────────────────────────
 * cmd_help
 * ───────────────────────────────────────────── */
int cmd_help(int argc, char **argv) {
    /* Se viene passato un comando specifico, mostra solo quello */
    const char *topic = (argc >= 2) ? argv[1] : NULL;

#define H_BOLD  "\033[1m"
#define H_DIM   "\033[2m"
#define H_YEL   "\033[33m"
#define H_CYN   "\033[36m"
#define H_RST   "\033[0m"
#define H_GRN   "\033[32m"

    if (!topic) {
        printf(H_BOLD "VHDS — Virtual Home Directory System\n" H_RST);
        printf(H_DIM  "  Type 'help <command>' for detailed usage.\n\n" H_RST);

        printf(H_YEL H_BOLD "  Authentication\n" H_RST);
        printf("    " H_BOLD "create_user" H_RST " <username> <perm>    Create a new user (requires root)\n");
        printf("    " H_BOLD "login" H_RST "       <username>           Log in as a user\n");
        printf("    " H_BOLD "exit" H_RST "                             Disconnect from server\n");
        printf("    " H_BOLD "ping" H_RST "                             Check server connectivity\n");

        printf(H_YEL H_BOLD "\n  Filesystem\n" H_RST);
        printf("    " H_BOLD "create" H_RST "  [-d] <path> <perm>      Create file or " H_DIM "[-d]" H_RST " directory\n");
        printf("    " H_BOLD "chmod" H_RST "   <path> <perm>           Change permissions (octal, e.g. 0644)\n");
        printf("    " H_BOLD "move" H_RST "    <src>  <dst>            Rename or move file\n");
        printf("    " H_BOLD "delete" H_RST "  <path>                  Remove file or empty directory\n");
        printf("    " H_BOLD "cd" H_RST "      <path>                  Change working directory\n");
        printf("    " H_BOLD "list" H_RST "    [path]                  List directory contents\n");
        printf("    " H_BOLD "read" H_RST "    [-offset=N] [-n=N] <path>  Read file (with optional offset/limit)\n");
        printf("    " H_BOLD "write" H_RST "   [-offset=N] [-a] <path> Write to file (stdin, end with '.')\n");

        printf(H_YEL H_BOLD "\n  Transfer\n" H_RST);
        printf("    " H_BOLD "upload" H_RST "   [-b] <local>  <remote> Upload local file to server\n");
        printf("    " H_BOLD "download" H_RST " [-b] <remote> <local>  Download file from server\n");
        printf("    " H_BOLD "transfer_request" H_RST " <file> <user>  Send file to another user (P2P)\n");
        printf("    " H_BOLD "accept" H_RST "  <dir> <ID>              Accept incoming transfer\n");
        printf("    " H_BOLD "reject" H_RST "  <ID>                    Reject incoming transfer\n");

        printf(H_YEL H_BOLD "\n  History\n" H_RST);
        printf("    " H_BOLD "history" H_RST "                         Show command history\n");
        printf("    " H_BOLD "!!" H_RST "                              Repeat last command\n");
        printf("\n");
        return 0;
    }

    /* Aiuto per comando specifico */
    if (strcmp(topic, "read") == 0) {
        printf(H_BOLD "read" H_RST " — Read a file from the virtual filesystem\n\n");
        printf("  " H_CYN "read" H_RST " [" H_DIM "-offset=N" H_RST "] [" H_DIM "-n=N" H_RST "] <path>\n\n");
        printf("  Options:\n");
        printf("    " H_DIM "-offset=N" H_RST "   Start reading at byte offset N\n");
        printf("    " H_DIM "-n=N" H_RST "        Read at most N bytes\n\n");
        printf("  Examples:\n");
        printf("    read notes.txt\n");
        printf("    read -offset=100 notes.txt\n");
        printf("    read -n=512 notes.txt\n");
        printf("    read -offset=100 -n=50 notes.txt\n");
    } else if (strcmp(topic, "write") == 0) {
        printf(H_BOLD "write" H_RST " — Write to a file (creates if not exists)\n\n");
        printf("  " H_CYN "write" H_RST " [" H_DIM "-offset=N" H_RST "] [" H_DIM "-a" H_RST "] <path>\n\n");
        printf("  Options:\n");
        printf("    " H_DIM "-offset=N" H_RST "   Overwrite starting at byte offset N\n");
        printf("    " H_DIM "-a" H_RST "          Append mode: add text to end of file\n\n");
        printf("  Type content line by line, then a single '.' to finish.\n\n");
        printf("  Examples:\n");
        printf("    write myfile.txt          # overwrite\n");
        printf("    write -a myfile.txt       # append to end\n");
        printf("    write -offset=10 log.txt  # overwrite from byte 10\n");
    } else if (strcmp(topic, "upload") == 0 || strcmp(topic, "download") == 0) {
        printf(H_BOLD "%s" H_RST " — Transfer files %s the server\n\n",
               topic, strcmp(topic,"upload")==0 ? "to" : "from");
        printf("  " H_CYN "%s" H_RST " [" H_DIM "-b" H_RST "] <src> <dst>\n\n", topic);
        printf("  Options:\n");
        printf("    " H_DIM "-b" H_RST "    Background mode: returns to prompt immediately\n\n");
        printf("  Background jobs complete asynchronously. " H_GRN "exit" H_RST " waits for all to finish.\n");
    } else if (strcmp(topic, "transfer_request") == 0 ||
               strcmp(topic, "accept") == 0 ||
               strcmp(topic, "reject") == 0) {
        printf(H_BOLD "P2P Transfer\n" H_RST);
        printf("  1. Sender:    " H_CYN "transfer_request" H_RST " <file> <recipient>\n");
        printf("  2. Recipient: " H_CYN "accept" H_RST " <destination_dir> <transfer_ID>\n");
        printf("             or " H_CYN "reject" H_RST " <transfer_ID>\n\n");
        printf("  Transfer requests expire automatically after timeout.\n");
    } else if (strcmp(topic, "list") == 0) {
        printf(H_BOLD "list" H_RST " — List directory contents\n\n");
        printf("  " H_CYN "list" H_RST " [path]\n\n");
        printf("  Shows: name, permissions, size, last modified date.\n");
        printf("  Default path is current working directory.\n\n");
        printf("  Examples:\n");
        printf("    list\n");
        printf("    list /\n");
        printf("    list subdir/\n");
    } else {
        printf("No detailed help for '%s'. Try 'help' for an overview.\n", topic);
    }

    printf(H_RST);
    return 0;
}
