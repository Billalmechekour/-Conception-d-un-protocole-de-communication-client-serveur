CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -pthread -D_XOPEN_SOURCE=700
LDFLAGS = -pthread

SRC = src
BIN = bin

COMMON_SRCS = $(SRC)/common.c
SERVER_SRCS  = $(SRC)/tftp_server_threaded.c $(SRC)/common.c $(SRC)/file_lock.c
SELECT_SRCS  = $(SRC)/tftp_server_select.c $(SRC)/common.c $(SRC)/file_lock.c
CLIENT_SRCS  = $(SRC)/tftp_client.c $(SRC)/common.c

all: dirs $(BIN)/tftp_server_threaded $(BIN)/tftp_server_select $(BIN)/tftp_client

dirs:
	mkdir -p $(BIN)

$(BIN)/tftp_server_threaded: $(SERVER_SRCS) $(SRC)/common.h $(SRC)/file_lock.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS)

$(BIN)/tftp_server_select: $(SELECT_SRCS) $(SRC)/common.h $(SRC)/file_lock.h
	$(CC) $(CFLAGS) -o $@ $(SELECT_SRCS) $(LDFLAGS)

$(BIN)/tftp_client: $(CLIENT_SRCS) $(SRC)/common.h
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS)

clean:
	rm -rf $(BIN)

.PHONY: all dirs clean