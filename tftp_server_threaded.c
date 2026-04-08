/*
 * tftp_server_threaded.c  —  Serveur TFTP multi-clients pthread (étapes 3–5)
 *
 * Nouveautés étapes 4 & 5 :
 *   - Négociation d'options RFC 2347 (OACK)
 *   - Option bigfile    : rollover des numéros de bloc (étape 4)
 *   - Option windowsize : fenêtre glissante RFC 7440 (étape 5)
 */

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
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

/* ──────────────────────────────────────────────────
 *  Utilitaires internes
 * ────────────────────────────────────────────────── */

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s <listen_port> <root_dir> [timeout_ms]\n"
        "Example: %s 6969 /tmp/tftp 1000\n", p, p);
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
    size_t  len = tftp_build_error(pkt, sizeof(pkt), code, msg);
    if (!len) return -1;
    return (sendto(sock, pkt, len, 0, (const struct sockaddr *)to, tolen) < 0) ? -1 : 0;
}

static int parse_rrq_wrq(const uint8_t *buf, size_t len,
                          char *filename, size_t fnsz,
                          char *mode, size_t mdsz,
                          size_t *opts_offset) {
    if (len < 4) return -1;
    size_t i = 2, fn_i = 0;
    while (i < len && buf[i] != '\0') {
        if (fn_i + 1 >= fnsz) return -1;
        filename[fn_i++] = (char)buf[i++];
    }
    if (i >= len) return -1;
    filename[fn_i] = '\0'; i++;

    size_t md_i = 0;
    while (i < len && buf[i] != '\0') {
        if (md_i + 1 >= mdsz) return -1;
        mode[md_i++] = (char)buf[i++];
    }
    if (buf[i] != '\0') return -1;
    mode[md_i] = '\0'; i++;

    if (opts_offset) *opts_offset = i; /* position des options eventuelles */
    return 0;
}

static int mode_supported(const char *mode) {
    return (strcasecmp(mode, "octet") == 0) || (strcasecmp(mode, "netascii") == 0);
}

/* Retourne le numero de bloc 16 bits a mettre sur le fil */
static inline uint16_t wire_blk(uint32_t b32) { return (uint16_t)(b32 & 0xFFFFu); }

/* ══════════════════════════════════════════════════════
 *  handle_rrq — envoi d'un fichier au client (GET)
 *  Supporte : bigfile + windowsize
 * ══════════════════════════════════════════════════════ */
static int handle_rrq(int sock,
                      const struct sockaddr_in *cli, socklen_t clilen,
                      const char *rootdir, const char *filename,
                      int timeout_ms, const tftp_options_t *opts) {
    /* Verrou en lecture (plusieurs GET simultanes sur le meme fichier) */
    file_lock_handle_t *lh = file_lock_acquire(filename, FILE_LOCK_READ);
    if (!lh) { send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "Lock error"); return 0; }

    char root[512];
    if (ensure_dir_slash(root, sizeof(root), rootdir) < 0) {
        file_lock_release(lh); return -1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s%s", root, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send_error(sock, cli, clilen, TFTP_ERR_FILE_NOT_FOUND, "File not found");
        file_lock_release(lh); return 0;
    }

    /* ── Negociation d'options : envoyer un OACK si options demandees ── */
    int needs_oack   = (opts->bigfile || opts->windowsize > 1);
    int has_bigfile  = opts->bigfile;
    int has_winsize  = (opts->windowsize > 1);

    if (needs_oack) {
        uint8_t oack[TFTP_MAX_OPT_PKT];
        size_t  oacklen = tftp_build_oack(oack, sizeof(oack), opts, has_bigfile, has_winsize);
        if (!oacklen) {
            send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "OACK build failed");
            fclose(fp); file_lock_release(lh); return 0;
        }

        /* Envoyer OACK et attendre ACK(0) du client */
        int retries = 0;
        int ack0_ok = 0;
        uint8_t in[TFTP_MAX_PKT];
        for (;;) {
            sendto(sock, oack, oacklen, 0, (const struct sockaddr *)cli, clilen);
            int w = wait_readable(sock, timeout_ms);
            if (w <= 0) {
                if (w == 0 && ++retries < TFTP_MAX_RETRIES) continue;
                fclose(fp); file_lock_release(lh); return 0;
            }
            struct sockaddr_in src; socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(sock, in, sizeof(in), 0,
                                 (struct sockaddr *)&src, &srclen);
            if (n < 4) continue;
            if (src.sin_addr.s_addr != cli->sin_addr.s_addr ||
                src.sin_port         != cli->sin_port) {
                send_error(sock, &src, srclen, TFTP_ERR_UNKNOWN_TID, "Unknown TID");
                continue;
            }
            uint16_t op = tftp_rd_u16(in);
            if (op == TFTP_OP_ERROR) { fclose(fp); file_lock_release(lh); return 0; }
            if (op == TFTP_OP_ACK && tftp_rd_u16(in + 2) == 0) { ack0_ok = 1; break; }
        }
        if (!ack0_ok) { fclose(fp); file_lock_release(lh); return 0; }
    }

    /* ─── Boucle d'envoi des blocs DATA ─── */
    int      windowsize = opts->windowsize;
    uint32_t block32    = 0;   /* compteur logique 32 bits (bigfile) */
    uint8_t  in[TFTP_MAX_PKT];

    /* Tampon de la fenetre */
    typedef struct { uint8_t pkt[TFTP_MAX_PKT]; size_t len; } slot_t;
    slot_t *window = (slot_t *)calloc((size_t)windowsize, sizeof(slot_t));
    if (!window) {
        send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "Out of memory");
        fclose(fp); file_lock_release(lh); return 0;
    }

    int done = 0;
    while (!done) {
        /* Lire jusqu'a windowsize blocs depuis le fichier */
        int   nslots = 0;
        int   eof    = 0;
        for (int s = 0; s < windowsize && !eof; s++) {
            uint8_t filebuf[TFTP_BLOCK_SIZE];
            size_t rd = fread(filebuf, 1, sizeof(filebuf), fp);
            if (ferror(fp)) {
                send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "Read error");
                free(window); fclose(fp); file_lock_release(lh); return 0;
            }
            block32++;
            size_t pktlen = tftp_build_data(window[s].pkt, TFTP_MAX_PKT,
                                            wire_blk(block32), filebuf, rd);
            if (!pktlen) { free(window); fclose(fp); file_lock_release(lh); return -1; }
            window[s].len = pktlen;
            nslots++;
            if (rd < TFTP_BLOCK_SIZE) eof = 1;
        }

        /* Envoyer tous les paquets de la fenetre */
        for (int s = 0; s < nslots; s++) {
            if (sendto(sock, window[s].pkt, window[s].len, 0,
                       (const struct sockaddr *)cli, clilen) < 0) {
                perror("sendto(DATA)");
                free(window); fclose(fp); file_lock_release(lh); return -1;
            }
        }

        /* Attendre ACK du dernier bloc de la fenetre */
        uint16_t expected_ack = wire_blk(block32);
        int retries = 0;
        for (;;) {
            int w = wait_readable(sock, timeout_ms);
            if (w < 0) { perror("select");
                         free(window); fclose(fp); file_lock_release(lh); return -1; }
            if (w == 0) {
                if (++retries >= TFTP_MAX_RETRIES) {
                    free(window); fclose(fp); file_lock_release(lh); return 0;
                }
                /* Retransmettre la fenetre entiere */
                for (int s = 0; s < nslots; s++) {
                    sendto(sock, window[s].pkt, window[s].len, 0,
                           (const struct sockaddr *)cli, clilen);
                }
                continue;
            }

            struct sockaddr_in src; socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&src, &srclen);
            if (n < 4) continue;

            if (src.sin_addr.s_addr != cli->sin_addr.s_addr ||
                src.sin_port         != cli->sin_port) {
                send_error(sock, &src, srclen, TFTP_ERR_UNKNOWN_TID, "Unknown TID");
                continue;
            }

            uint16_t op = tftp_rd_u16(in);
            if (op == TFTP_OP_ERROR) { free(window); fclose(fp); file_lock_release(lh); return 0; }
            if (op != TFTP_OP_ACK) continue;

            uint16_t ackblk = tftp_rd_u16(in + 2);
            if (ackblk == expected_ack) break; /* fenetre acquittee */
            /* ACK partiel en cas de perte : continuer d'attendre */
        }

        if (eof) done = 1;
    }

    free(window);
    fclose(fp);
    file_lock_release(lh);
    return 0;
}

/* ══════════════════════════════════════════════════════
 *  handle_wrq — reception d'un fichier du client (PUT)
 *  Supporte : bigfile + windowsize
 * ══════════════════════════════════════════════════════ */
static int handle_wrq(int sock,
                      const struct sockaddr_in *cli, socklen_t clilen,
                      const char *rootdir, const char *filename,
                      int timeout_ms, const tftp_options_t *opts) {
    /* Verrou en ecriture (exclusif) */
    file_lock_handle_t *lh = file_lock_acquire(filename, FILE_LOCK_WRITE);
    if (!lh) { send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "Lock error"); return 0; }

    char root[512];
    if (ensure_dir_slash(root, sizeof(root), rootdir) < 0) {
        file_lock_release(lh); return -1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s%s", root, filename);

    /* Creation atomique (O_EXCL) pour eviter la race entre deux WRQ */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            send_error(sock, cli, clilen, TFTP_ERR_FILE_EXISTS, "File exists");
        else
            send_error(sock, cli, clilen, TFTP_ERR_ACCESS_VIOL, "Cannot create file");
        file_lock_release(lh); return 0;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        send_error(sock, cli, clilen, TFTP_ERR_ACCESS_VIOL, "fdopen failed");
        file_lock_release(lh); return 0;
    }

    /* ── Negociation d'options : OACK ou ACK(0) ── */
    int needs_oack  = (opts->bigfile || opts->windowsize > 1);
    int has_bigfile = opts->bigfile;
    int has_winsize = (opts->windowsize > 1);

    /* 
     * En mode avec options, le serveur envoie OACK en guise d'ACK(0).
     * En cas de timeout AVANT reception de DATA(1), il doit retransmettre
     * l'OACK (et non ACK(0)), car le client attend toujours l'OACK.
     * Des que DATA(1) est recu et accepte, on bascule sur last_ack DATA. */
    uint8_t oack_handshake[TFTP_MAX_OPT_PKT];
    size_t  oack_handshake_len = 0;
    uint8_t last_ack[4];

    if (needs_oack) {
        oack_handshake_len = tftp_build_oack(oack_handshake, sizeof(oack_handshake),
                                              opts, has_bigfile, has_winsize);
        if (!oack_handshake_len) {
            send_error(sock, cli, clilen, TFTP_ERR_UNDEF, "OACK build failed");
            fclose(fp); file_lock_release(lh); return 0;
        }
        sendto(sock, oack_handshake, oack_handshake_len, 0,
               (const struct sockaddr *)cli, clilen);
    } else {
        tftp_build_ack(last_ack, sizeof(last_ack), 0);
        sendto(sock, last_ack, 4, 0, (const struct sockaddr *)cli, clilen);
    }

    /* ─── Boucle de reception des blocs DATA ─── */
    uint32_t expected32 = 1;
    int      windowsize = opts->windowsize;
    uint8_t  in[TFTP_MAX_PKT];
    int      retries    = 0;
    int      done       = 0;

    while (!done) {
        /* Recevoir jusqu'a windowsize blocs */
        int pkts_recv = 0;
        uint16_t last_good_wire = wire_blk(expected32 - 1);

        for (;;) {
            int w = wait_readable(sock, timeout_ms);
            if (w < 0) { perror("select"); fclose(fp); file_lock_release(lh); return -1; }
            if (w == 0) {
                if (++retries >= TFTP_MAX_RETRIES) {
                    fclose(fp); file_lock_release(lh); return 0;
                }
                /* Retransmettre : OACK si handshake pas encore complete,
                 * sinon le dernier ACK DATA. */
                if (needs_oack && expected32 == 1)
                    sendto(sock, oack_handshake, oack_handshake_len, 0,
                           (const struct sockaddr *)cli, clilen);
                else
                    sendto(sock, last_ack, 4, 0, (const struct sockaddr *)cli, clilen);
                continue;
            }
            retries = 0;

            struct sockaddr_in src; socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&src, &srclen);
            if (n < 4) continue;

            if (src.sin_addr.s_addr != cli->sin_addr.s_addr ||
                src.sin_port         != cli->sin_port) {
                send_error(sock, &src, srclen, TFTP_ERR_UNKNOWN_TID, "Unknown TID");
                continue;
            }

            uint16_t op = tftp_rd_u16(in);
            if (op == TFTP_OP_ERROR) { fclose(fp); file_lock_release(lh); return 0; }
            if (op != TFTP_OP_DATA)   continue;

            uint16_t blk_wire = tftp_rd_u16(in + 2);
            size_t   dlen     = (size_t)n - 4;

            if (blk_wire == wire_blk(expected32)) {
                /* Bloc attendu : ecrire les donnees */
                if (dlen > 0) {
                    if (fwrite(in + 4, 1, dlen, fp) != dlen) {
                        send_error(sock, cli, clilen, TFTP_ERR_DISK_FULL, "Write error");
                        fclose(fp); file_lock_release(lh); return 0;
                    }
                }
                last_good_wire = blk_wire;
                expected32++;
                pkts_recv++;
                /* Construire l'ACK DATA (bascule du handshake OACK vers ACK DATA) */
                tftp_build_ack(last_ack, sizeof(last_ack), blk_wire);

                if (dlen < TFTP_BLOCK_SIZE) {
                    /* Dernier bloc : ACK final */
                    tftp_build_ack(last_ack, sizeof(last_ack), blk_wire);
                    sendto(sock, last_ack, 4, 0, (const struct sockaddr *)cli, clilen);
                    done = 1;
                    break;
                }

                /* Si fin de fenetre : envoyer ACK du dernier bloc recu */
                if (pkts_recv >= windowsize) {
                    tftp_build_ack(last_ack, sizeof(last_ack), blk_wire);
                    sendto(sock, last_ack, 4, 0, (const struct sockaddr *)cli, clilen);
                    pkts_recv = 0;
                    break; /* attendre la prochaine fenetre */
                }
                /* Continuer a recevoir dans cette fenetre */

            } else if (blk_wire == wire_blk(expected32 - 1)) {
                /* Bloc duplique : renvoyer l'ACK precedent */
                sendto(sock, last_ack, 4, 0, (const struct sockaddr *)cli, clilen);
            }
            /* Autres numeros : ignorer */
            (void)last_good_wire;
        }
    }

    fclose(fp);
    file_lock_release(lh);
    return 0;
}

/* ══════════════════════════════════════════════════════
 *  Thread worker
 * ══════════════════════════════════════════════════════ */
typedef struct {
    uint16_t         op;
    struct sockaddr_in cli;
    socklen_t          clilen;
    char               rootdir [512];
    char               filename[256];
    int                timeout_ms;
    tftp_options_t     opts;     /* options negociees avec le client */
} worker_args_t;

static void *worker_thread(void *arg) {
    worker_args_t *w = (worker_args_t *)arg;
    pthread_detach(pthread_self());

    int tsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tsock < 0) { free(w); return NULL; }

    struct sockaddr_in taddr;
    memset(&taddr, 0, sizeof(taddr));
    taddr.sin_family      = AF_INET;
    taddr.sin_addr.s_addr = htonl(INADDR_ANY);
    taddr.sin_port        = htons(0); /* port ephemere (TID) */

    if (bind(tsock, (struct sockaddr *)&taddr, sizeof(taddr)) < 0) {
        perror("bind(tsock)"); close(tsock); free(w); return NULL;
    }

    if (w->op == TFTP_OP_RRQ)
        handle_rrq(tsock, &w->cli, w->clilen, w->rootdir, w->filename,
                   w->timeout_ms, &w->opts);
    else
        handle_wrq(tsock, &w->cli, w->clilen, w->rootdir, w->filename,
                   w->timeout_ms, &w->opts);

    close(tsock);
    free(w);
    return NULL;
}

/* ══════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    uint16_t    port      = (uint16_t)atoi(argv[1]);
    const char *rootdir   = argv[2];
    int         timeout_ms = (argc >= 4) ? atoi(argv[3]) : 1000;

    int lsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(lsock); return 1;
    }

    fprintf(stderr, "[threaded] TFTP server listening on UDP %u, root=%s\n", port, rootdir);
    fprintf(stderr, "Options supportees : bigfile, windowsize (1..%d)\n", TFTP_MAX_WINDOW);

    for (;;) {
        uint8_t buf[TFTP_MAX_OPT_PKT];
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);

        ssize_t n = recvfrom(lsock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &clilen);
        if (n < 0) { if (errno == EINTR) continue; perror("recvfrom"); continue; }
        if (n < 2) continue;

        uint16_t op = tftp_rd_u16(buf);
        if (op != TFTP_OP_RRQ && op != TFTP_OP_WRQ) {
            send_error(lsock, &cli, clilen, TFTP_ERR_ILLEGAL_OP, "Illegal operation");
            continue;
        }

        char filename[256], mode[32];
        size_t opts_offset = 0;
        if (parse_rrq_wrq(buf, (size_t)n, filename, sizeof(filename),
                          mode, sizeof(mode), &opts_offset) < 0) {
            send_error(lsock, &cli, clilen, TFTP_ERR_ILLEGAL_OP, "Malformed request");
            continue;
        }
        if (!mode_supported(mode)) {
            send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF, "Unsupported mode");
            continue;
        }
        if (!filename_is_safe(filename)) {
            send_error(lsock, &cli, clilen, TFTP_ERR_ACCESS_VIOL, "Unsafe filename");
            continue;
        }

        /* Parser les options eventuelles (etapes 4 & 5) */
        tftp_options_t opts;
        tftp_options_init(&opts);
        if (opts_offset < (size_t)n) {
            int parse_rc = tftp_parse_options(buf, (size_t)n, opts_offset, &opts);
            if (parse_rc < 0) {
                /* valeur bigfile invalide -> ERROR */
                send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF,
                           "Invalid bigfile option value");
                continue;
            }
            if (opts.bigfile || opts.windowsize > 1) {
                fprintf(stderr, "[server] Requete %s fichier=%s bigfile=%d windowsize=%d\n",
                        (op == TFTP_OP_RRQ) ? "RRQ" : "WRQ",
                        filename, opts.bigfile, opts.windowsize);
            }
        }

        /* Allouer les arguments du thread */
        worker_args_t *w = (worker_args_t *)calloc(1, sizeof(*w));
        if (!w) {
            send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF, "Server busy");
            continue;
        }
        w->op         = op;
        w->cli        = cli;
        w->clilen     = clilen;
        w->timeout_ms = timeout_ms;
        w->opts       = opts;
        strncpy(w->rootdir,  rootdir,  sizeof(w->rootdir)  - 1);
        strncpy(w->filename, filename, sizeof(w->filename) - 1);

        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_thread, w) != 0) {
            perror("pthread_create");
            free(w);
            send_error(lsock, &cli, clilen, TFTP_ERR_UNDEF, "Cannot create thread");
        }
    }

    close(lsock);
    file_lock_table_destroy();
    return 0;
}
