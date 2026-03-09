# ─────────────────────────────────────────────
# VHDS Makefile
# ─────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -Iinclude -Iclient
DBGFLAGS= -DDEBUG -g -O0

# ── Sorgenti server ───────────────────────────
SRV_SRCS = server/main_server.c \
            server/session.c    \
            server/sync.c       \
            server/filesystem.c \
            server/transfer.c   \
            server/utils.c      \
            server/network.c

# ── Sorgenti client ───────────────────────────
CLI_SRCS = client/main_client.c   \
            client/history.c      \
            client/notify.c       \
            client/cmd_auth.c     \
            client/cmd_fs.c       \
            client/cmd_transfer.c \
            client/transfer.c     \
            server/utils.c        \
            server/network.c

SRV_OBJS = $(SRV_SRCS:.c=.o)
CLI_OBJS = $(CLI_SRCS:.c=.o)

.PHONY: all debug clean

all: Server Client
	@echo ">>> Build complete"

debug: CFLAGS += $(DBGFLAGS)
debug: all

Server: $(SRV_OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo ">>> Built Server"

Client: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo ">>> Built Client"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f server/*.o client/*.o Server Client
	@echo ">>> Cleaned"
