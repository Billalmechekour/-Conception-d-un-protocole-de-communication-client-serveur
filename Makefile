CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -pthread -D_XOPEN_SOURCE=700
LDFLAGS = -pthread

BIN = bin

COMMON_SRCS = common.c
SERVER_SRCS = tftp_server_threaded.c common.c file_lock.c
SELECT_SRCS = tftp_server_select.c common.c file_lock.c
CLIENT_SRCS = tftp_client.c common.c

all: dirs $(BIN)/tftp_server_threaded $(BIN)/tftp_server_select $(BIN)/tftp_client

dirs:
	mkdir -p $(BIN)

$(BIN)/tftp_server_threaded: $(SERVER_SRCS) common.h file_lock.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS)

$(BIN)/tftp_server_select: $(SELECT_SRCS) common.h file_lock.h
	$(CC) $(CFLAGS) -o $@ $(SELECT_SRCS) $(LDFLAGS)

$(BIN)/tftp_client: $(CLIENT_SRCS) common.h
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS)

clean:
	rm -rf $(BIN)

.PHONY: all dirs clean
