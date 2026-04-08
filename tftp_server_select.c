#include "common.h"
#include "file_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s <listen_port> <root_dir> [timeout_ms]\n"
        "Example: %s 6969 /tmp/tftp 1000\n"
        "Note: port 69 requires root.\n", p, p);
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL);
}

static int ensure_dir_slash(char *dst, size_t dstsz, const char *root) {
    size_t n = strlen(root);
    if (n + 2 > dstsz) return -1;
    strcpy(dst, root);
    if (dst[n-1] != '/') strcat(dst, "/");
    return 0;
}

static int send_error(int sock, const struct sockaddr_in *to, socklen_t tolen,
                      uint16_t code, const char *msg) {
    uint8_t pkt[TFTP_MAX_PKT];
    size_t len = tftp_build_error(pkt, sizeof(pkt), code, msg);
    if (!len) return -1;
    return (sendto(sock, pkt, len, 0, (struct sockaddr*)to, tolen) < 0) ? -1 : 0;
}

static int parse_rrq_wrq(const uint8_t *buf, size_t len,
                         char *filename, size_t fnsz,
                         char *mode, size_t mdsz) {
    if (len < 4) return -1;

    size_t i = 2, fn_i = 0;
    while (i < len && buf[i] != '\0') {
        if (fn_i + 1 >= fnsz) return -1;
        filename[fn_i++] = (char)buf[i++];
    }
    if (i >= len || buf[i] != '\0') return -1;
    filename[fn_i] = '\0';
    i++;

    size_t md_i = 0;
    while (i < len && buf[i] != '\0') {
        if (md_i + 1 >= mdsz) return -1;
        mode[md_i++] = (char)buf[i++];
    }
    if (i >= len || buf[i] != '\0') return -1;
    mode[md_i] = '\0';
    return 0;
}

static int mode_supported(const char *mode) {
    return mode && ((strcasecmp(mode, "octet") == 0) || (strcasecmp(mode, "netascii") == 0));
}

typedef enum {
    XFER_RRQ_WAIT_ACK = 0,
    XFER_WRQ_WAIT_DATA = 1
} xfer_state_t;

typedef struct transfer {
    int sock;
    xfer_state_t state;

    struct sockaddr_in peer;
    socklen_t peerlen;

    char filename[256];
    char path[1024];

    FILE *fp;
    file_lock_handle_t *lockh;

    int timeout_ms;
    int retries;

    /* RRQ: block = bloc courant envoyé (attend ACK block) */
    /* WRQ: block = prochain bloc attendu (expected) */
    uint16_t block;

    size_t last_payload_len; /* RRQ: taille data du dernier DATA */

    uint8_t last_sent[TFTP_MAX_PKT];
    size_t last_sent_len;
    long long last_sent_at;

    struct transfer *next;
} transfer_t;

static void transfer_free(transfer_t **head, transfer_t *t) {
    if (!t) return;

    /* unlink */
    transfer_t **pp = head;
    while (*pp) {
        if (*pp == t) { *pp = t->next; break; }
        pp = &(*pp)->next;
    }

    if (t->fp) fclose(t->fp);
    if (t->sock >= 0) close(t->sock);
    if (t->lockh) file_lock_release(t->lockh);

    free(t);
}

static int xfer_send(transfer_t *t, const uint8_t *buf, size_t len, int reset_retries) {
    if (sendto(t->sock, buf, len, 0, (struct sockaddr*)&t->peer, t->peerlen) < 0) return -1;
    memcpy(t->last_sent, buf, len);
    t->last_sent_len = len;
    t->last_sent_at = now_ms();
    if (reset_retries) t->retries = 0;
    return 0;
}

static int rrq_send_block(transfer_t *t) {
    uint8_t data[TFTP_BLOCK_SIZE];
    size_t rd = fread(data, 1, sizeof(data), t->fp);
    if (ferror(t->fp)) return -1;

    uint8_t pkt[TFTP_MAX_PKT];
    size_t pktlen = tftp_build_data(pkt, sizeof(pkt), t->block, data, rd);
    if (!pktlen) return -1;

    t->last_payload_len = rd;
    return xfer_send(t, pkt, pktlen, 1);
}

static transfer_t* create_transfer_socket(const struct sockaddr_in *cli, socklen_t clilen) {
    transfer_t *t = (transfer_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sock < 0) { free(t); return NULL; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(0);

    if (bind(t->sock, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(t->sock);
        free(t);
        return NULL;
    }

    t->peer = *cli;
    t->peerlen = clilen;
    t->fp = NULL;
    t->lockh = NULL;
    t->retries = 0;
    t->last_sent_len = 0;
    t->last_sent_at = now_ms();
    return t;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    uint16_t port = (uint16_t)atoi(argv[1]);
    const char *rootdir = argv[2];
    int timeout_ms = (argc >= 4) ? atoi(argv[3]) : 1000;

    char root[512];
    if (ensure_dir_slash(root, sizeof(root), rootdir) < 0) {
        fprintf(stderr, "Invalid root_dir\n");
        return 1;
    }

    int lsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lsock);
        return 1;
    }

    fprintf(stderr, "[select] TFTP server listening on UDP %u, root=%s\n", port, rootdir);

    transfer_t *xfers = NULL;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lsock, &rfds);
        int maxfd = lsock;

        long long now = now_ms();
        long long min_rem = -1;

        for (transfer_t *t = xfers; t; t = t->next) {
            FD_SET(t->sock, &rfds);
            if (t->sock > maxfd) maxfd = t->sock;

            long long elapsed = now - t->last_sent_at;
            long long rem = (long long)t->timeout_ms - elapsed;
            if (rem < 0) rem = 0;
            if (min_rem < 0 || rem < min_rem) min_rem = rem;
        }

        struct timeval tv;
        struct timeval *ptv = NULL;
        if (min_rem >= 0) {
            tv.tv_sec = (int)(min_rem / 1000);
            tv.tv_usec = (int)((min_rem % 1000) * 1000);
            ptv = &tv;
        }

        int r = select(maxfd + 1, &rfds, NULL, NULL, ptv);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        now = now_ms();

        /* 1) nouveaux clients */
        if (FD_ISSET(lsock, &rfds)) {
            uint8_t buf[TFTP_MAX_PKT];
            struct sockaddr_in cli;
            socklen_t clilen = sizeof(cli);

            ssize_t n = recvfrom(lsock, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &clilen);
            if (n >= 2) {
                uint16_t op = tftp_rd_u16(buf);

                if (op != TFTP_OP_RRQ && op != TFTP_OP_WRQ) {
                    (void)send_error(lsock, &cli, clilen, TFTP_ERR_ILLEGAL_OP, "Illegal operation");
                } else {
                    char filename[256], mode[32];
                    if (parse_rrq_wrq(buf, (size_t)n, filename, sizeof(filename), mode, sizeof(mode)) < 0) {
                        (void)send_error(lsock, &cli, clilen, TFTP_ERR_ILLEGAL_OP, "Malformed request");
                    } else if (!mode_supported(mode)) {
                        (void)send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF, "Unsupported mode");
                    } else if (!filename_is_safe(filename)) {
                        (void)send_error(lsock, &cli, clilen, TFTP_ERR_ACCESS_VIOL, "Unsafe filename");
                    } else {
                        transfer_t *t = create_transfer_socket(&cli, clilen);
                        if (!t) {
                            (void)send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF, "Server busy");
                        } else {
                            strncpy(t->filename, filename, sizeof(t->filename) - 1);
                            snprintf(t->path, sizeof(t->path), "%s%s", root, filename);
                            t->timeout_ms = timeout_ms;

                            if (op == TFTP_OP_RRQ) {
                                t->lockh = file_lock_acquire(filename, FILE_LOCK_READ);
                                if (!t->lockh) {
                                    (void)send_error(t->sock, &cli, clilen, TFTP_ERR_UNDEF, "Lock error");
                                    transfer_free(&xfers, t);
                                } else {
                                    t->fp = fopen(t->path, "rb");
                                    if (!t->fp) {
                                        (void)send_error(t->sock, &cli, clilen, TFTP_ERR_FILE_NOT_FOUND, "File not found");
                                        transfer_free(&xfers, t);
                                    } else {
                                        t->state = XFER_RRQ_WAIT_ACK;
                                        t->block = 1;
                                        if (rrq_send_block(t) < 0) {
                                            transfer_free(&xfers, t);
                                        } else {
                                            t->next = xfers;
                                            xfers = t;
                                        }
                                    }
                                }
                            } else {
                                t->lockh = file_lock_acquire(filename, FILE_LOCK_WRITE);
                                if (!t->lockh) {
                                    (void)send_error(t->sock, &cli, clilen, TFTP_ERR_UNDEF, "Lock error");
                                    transfer_free(&xfers, t);
                                } else {
                                    int fd = open(t->path, O_WRONLY | O_CREAT | O_EXCL, 0644);
                                    if (fd < 0) {
                                        if (errno == EEXIST) {
                                            (void)send_error(t->sock, &cli, clilen, TFTP_ERR_FILE_EXISTS, "File exists");
                                        } else {
                                            (void)send_error(t->sock, &cli, clilen, TFTP_ERR_ACCESS_VIOL, "Cannot create file");
                                        }
                                        transfer_free(&xfers, t);
                                    } else {
                                        t->fp = fdopen(fd, "wb");
                                        if (!t->fp) {
                                            close(fd);
                                            (void)send_error(t->sock, &cli, clilen, TFTP_ERR_ACCESS_VIOL, "Cannot open file");
                                            transfer_free(&xfers, t);
                                        } else {
                                            t->state = XFER_WRQ_WAIT_DATA;
                                            t->block = 1; /* expected */
                                            uint8_t ack0[4];
                                            tftp_build_ack(ack0, sizeof(ack0), 0);
                                            if (xfer_send(t, ack0, 4, 1) < 0) {
                                                transfer_free(&xfers, t);
                                            } else {
                                                t->next = xfers;
                                                xfers = t;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /* 2) traiter les transferts actifs */
        for (transfer_t *t = xfers; t; ) {
            transfer_t *next = t->next;

            if (FD_ISSET(t->sock, &rfds)) {
                uint8_t in[TFTP_MAX_PKT];
                struct sockaddr_in src;
                socklen_t srclen = sizeof(src);
                ssize_t n = recvfrom(t->sock, in, sizeof(in), 0, (struct sockaddr*)&src, &srclen);
                if (n >= 4) {
                    if (src.sin_addr.s_addr != t->peer.sin_addr.s_addr || src.sin_port != t->peer.sin_port) {
                        (void)send_error(t->sock, &src, srclen, TFTP_ERR_UNKNOWN_TID, "Unknown transfer ID");
                    } else {
                        uint16_t op = tftp_rd_u16(in);

                        if (op == TFTP_OP_ERROR) {
                            transfer_free(&xfers, t);
                        } else if (t->state == XFER_RRQ_WAIT_ACK) {
                            if (op == TFTP_OP_ACK) {
                                uint16_t ackblk = tftp_rd_u16(in + 2);
                                if (ackblk == t->block) {
                                    if (t->last_payload_len < TFTP_BLOCK_SIZE) {
                                        transfer_free(&xfers, t);
                                    } else {
                                        t->block++;
                                        if (rrq_send_block(t) < 0) {
                                            transfer_free(&xfers, t);
                                        }
                                    }
                                }
                            }
                        } else { /* WRQ */
                            if (op == TFTP_OP_DATA) {
                                uint16_t blk = tftp_rd_u16(in + 2);
                                size_t dlen = (size_t)n - 4;

                                if (blk == t->block) {
                                    if (dlen > 0) {
                                        if (fwrite(in + 4, 1, dlen, t->fp) != dlen) {
                                            (void)send_error(t->sock, &t->peer, t->peerlen, TFTP_ERR_DISK_FULL, "Write error");
                                            transfer_free(&xfers, t);
                                            t = next;
                                            continue;
                                        }
                                    }
                                    uint8_t ackp[4];
                                    tftp_build_ack(ackp, sizeof(ackp), blk);
                                    if (xfer_send(t, ackp, 4, 1) < 0) {
                                        transfer_free(&xfers, t);
                                    } else {
                                        t->block++;
                                        if (dlen < TFTP_BLOCK_SIZE) {
                                            transfer_free(&xfers, t);
                                        }
                                    }
                                } else if (blk == (uint16_t)(t->block - 1)) {
                                    (void)xfer_send(t, t->last_sent, t->last_sent_len, 0);
                                }
                            }
                        }
                    }
                }
            }

            t = next;
        }

        /* 3) timeouts / retransmissions */
        for (transfer_t *t = xfers; t; ) {
            transfer_t *next = t->next;
            long long elapsed = now - t->last_sent_at;
            if (elapsed >= (long long)t->timeout_ms) {
                t->retries++;
                if (t->retries >= TFTP_MAX_RETRIES) {
                    transfer_free(&xfers, t);
                } else {
                    (void)xfer_send(t, t->last_sent, t->last_sent_len, 0);
                }
            }
            t = next;
        }
    }

    close(lsock);
    file_lock_table_destroy();
    return 0;
}
