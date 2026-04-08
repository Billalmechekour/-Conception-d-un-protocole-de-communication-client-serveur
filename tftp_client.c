/*
 * tftp_client.c  —  Client TFTP  (étapes 1–5)
 *
 * Supporte :
 *   - GET / PUT standard (RFC 1350)
 *   - Option bigfile    (étape 4, RFC 2347)
 *   - Option windowsize (étape 5, RFC 7440)
 *
 * Usage :
 *   tftp_client get <ip> <remote> <local>  [port] [timeout_ms] [windowsize] [bigfile]
 *   tftp_client put <ip> <local>  <remote> [port] [timeout_ms] [windowsize] [bigfile]
 *
 *   windowsize : 1..64 (defaut 1 = stop-and-wait)
 *   bigfile    : 0|1   (defaut 0)
 */

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

/* ──────────────────────────────────────────────────
 *  Utilitaires internes
 * ────────────────────────────────────────────────── */

static void usage(const char *p) {
    fprintf(stderr,
        "Usage:\n"
        "  %s get <server_ip> <remote_file> <local_file>"
        " [port] [timeout_ms] [windowsize] [bigfile:0|1]\n"
        "  %s put <server_ip> <local_file>  <remote_file>"
        " [port] [timeout_ms] [windowsize] [bigfile:0|1]\n"
        "Defaults: port=69 timeout_ms=1000 windowsize=1 bigfile=0\n", p, p);
}

static int resolve_ipv4(const char *ip, struct sockaddr_in *out, uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    return (inet_pton(AF_INET, ip, &out->sin_addr) == 1) ? 0 : -1;
}

static int send_error_pkt(int sock, const struct sockaddr_in *to, socklen_t tolen,
                          uint16_t code, const char *msg) {
    uint8_t pkt[TFTP_MAX_PKT];
    size_t  len = tftp_build_error(pkt, sizeof(pkt), code, msg);
    if (!len) return -1;
    return (sendto(sock, pkt, len, 0, (const struct sockaddr *)to, tolen) < 0) ? -1 : 0;
}

/* ──────────────────────────────────────────────────
 *  Logique bigfile : numero de bloc 32 bits
 *
 *  Sur le reseau le bloc reste sur 16 bits (wire_block).
 *  En mode bigfile on autorise le rollover : apres 65535 → 0 → 1 …
 *  On suit le compteur logique 32 bits (block32) pour detecter
 *  correctement les doublons et les paquets hors ordre.
 * ────────────────────────────────────────────────── */

/* Retourne le numero de bloc 16 bits a mettre sur le fil */
static inline uint16_t wire_block(uint32_t block32) {
    return (uint16_t)(block32 & 0xFFFFu);
}

/*
 * Determine si blk_wire correspond au bloc attendu ou a un doublon,
 * en tenant compte du rollover potentiel en mode bigfile.
 * Retourne  1 si c'est le bloc attendu
 *           0 si c'est le bloc precedent (doublon)
 *          -1 sinon
 */
static int classify_block(uint16_t blk_wire, uint32_t expected32, int bigfile) {
    uint16_t exp_wire  = wire_block(expected32);
    uint16_t prev_wire = wire_block(expected32 - 1);

    if (blk_wire == exp_wire)  return  1;
    if (blk_wire == prev_wire) return  0;

    /* En mode bigfile, le rollover peut faire qu'expected_wire == 0
     * et prev_wire == 65535 ; les deux cas sont deja couverts. */
    (void)bigfile;
    return -1;
}

/* ══════════════════════════════════════════════════════
 *  TFTP GET  (RRQ)
 *  Supporte : bigfile + windowsize
 * ══════════════════════════════════════════════════════ */
static int tftp_get(const char *server_ip, const char *remote, const char *local,
                    uint16_t port, int timeout_ms, const tftp_options_t *req_opts) {
    int   sock = -1;
    FILE *fp   = NULL;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    if (resolve_ipv4(server_ip, &srv, port) < 0) {
        fprintf(stderr, "Invalid server IPv4: %s\n", server_ip);
        close(sock); return 1;
    }

    /* Construire le RRQ (avec options si demandees) */
    uint8_t out[TFTP_MAX_OPT_PKT];
    size_t  outlen;
    int using_opts = (req_opts && (req_opts->bigfile || req_opts->windowsize > 1));
    if (using_opts) {
        outlen = tftp_build_rrq_wrq_opts(out, sizeof(out), TFTP_OP_RRQ,
                                          remote, TFTP_MODE, req_opts);
    } else {
        outlen = tftp_build_rrq_wrq(out, sizeof(out), TFTP_OP_RRQ, remote, TFTP_MODE);
    }
    if (!outlen) { fprintf(stderr, "RRQ build failed\n"); close(sock); return 1; }

    fp = fopen(local, "wb");
    if (!fp) { perror("fopen"); close(sock); return 1; }

    /* Envoyer le RRQ */
    if (sendto(sock, out, outlen, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("sendto(RRQ)"); goto fail;
    }

    /* Options effectives apres negociation */
    tftp_options_t neg;
    tftp_options_init(&neg);

    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    int peer_locked = 0;       /* TID du serveur connu ? */
    uint8_t in[TFTP_MAX_PKT];
    ssize_t n;

    /* ─── Phase 1 : attente du premier paquet (OACK ou DATA 1) ─── */
    {
        int retries = 0;
        for (;;) {
            int w = wait_readable(sock, timeout_ms);
            if (w < 0) { perror("select"); goto fail; }
            if (w == 0) {
                if (++retries >= TFTP_MAX_RETRIES) {
                    fprintf(stderr, "Timeout waiting for first packet\n"); goto fail;
                }
                /* Retransmettre le RRQ */
                sendto(sock, out, outlen, 0, (struct sockaddr *)&srv, sizeof(srv));
                continue;
            }

            n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&peer, &peerlen);
            if (n < 4) continue;

            uint16_t op = tftp_rd_u16(in);

            if (op == TFTP_OP_ERROR) {
                fprintf(stderr, "TFTP ERROR %u: %s\n", tftp_rd_u16(in+2), (char*)(in+4));
                goto fail;
            }

            if (op == TFTP_OP_OACK) {
                /* Serveur supporte les options : les analyser */
                if (tftp_parse_options(in, (size_t)n, 2, &neg) < 0) {
                    send_error_pkt(sock, &peer, peerlen, TFTP_ERR_ILLEGAL_OP, "Bad OACK");
                    goto fail;
                }
                fprintf(stderr, "[client] OACK accepted: bigfile=%d windowsize=%d\n",
                        neg.bigfile, neg.windowsize);
                peer_locked = 1;

                /* Accuser reception de l'OACK avec ACK(0) */
                uint8_t ack0[4];
                tftp_build_ack(ack0, sizeof(ack0), 0);
                sendto(sock, ack0, 4, 0, (struct sockaddr *)&peer, peerlen);
                break;
            }

            if (op == TFTP_OP_DATA) {
                /* Serveur ignore les options : commencer a bloc 1 */
                uint16_t blk = tftp_rd_u16(in + 2);
                if (blk != 1) continue;
                peer_locked = 1;

                size_t dlen = (size_t)n - 4;
                if (dlen > 0 && fwrite(in + 4, 1, dlen, fp) != dlen) {
                    perror("fwrite"); goto fail;
                }
                uint8_t ack[4];
                tftp_build_ack(ack, sizeof(ack), 1);
                sendto(sock, ack, 4, 0, (struct sockaddr *)&peer, peerlen);

                if (dlen < TFTP_BLOCK_SIZE) { fclose(fp); close(sock); return 0; }
                break;
            }
        }
    }
    (void)peer_locked;

    /* ─── Phase 2 : reception des blocs (avec windowsize) ─── */
    {
        uint32_t expected32 = 2; /* prochain bloc attendu (logique 32 bits) */
        uint8_t  last_ack[4];
        int      done = 0;

        /* ACK du bloc 1 deja envoye ; memoriser pour retransmission */
        tftp_build_ack(last_ack, sizeof(last_ack), wire_block(expected32 - 1));

        int retries = 0;

        while (!done) {
            /* Avec windowsize, on attend jusqu'a neg.windowsize blocs */
            int pkts_in_window = 0;

            for (;;) {
                int w = wait_readable(sock, timeout_ms);
                if (w < 0) { perror("select"); goto fail; }
                if (w == 0) {
                    /* Timeout : retransmettre le dernier ACK */
                    if (++retries >= TFTP_MAX_RETRIES) {
                        fprintf(stderr, "Timeout: too many retries\n"); goto fail;
                    }
                    sendto(sock, last_ack, 4, 0, (struct sockaddr *)&peer, peerlen);
                    continue;
                }
                retries = 0;

                struct sockaddr_in src;
                socklen_t srclen = sizeof(src);
                n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&src, &srclen);
                if (n < 4) continue;

                /* Verifier TID */
                if (src.sin_addr.s_addr != peer.sin_addr.s_addr ||
                    src.sin_port         != peer.sin_port) {
                    send_error_pkt(sock, &src, srclen,
                                   TFTP_ERR_UNKNOWN_TID, "Unknown TID");
                    continue;
                }

                uint16_t op = tftp_rd_u16(in);
                if (op == TFTP_OP_ERROR) {
                    fprintf(stderr, "TFTP ERROR %u: %s\n",
                            tftp_rd_u16(in+2), (char*)(in+4));
                    goto fail;
                }
                if (op != TFTP_OP_DATA) continue;

                uint16_t blk_wire = tftp_rd_u16(in + 2);
                int cls = classify_block(blk_wire, expected32, neg.bigfile);

                if (cls == 1) {
                    /* Bloc attendu */
                    size_t dlen = (size_t)n - 4;
                    if (dlen > 0 && fwrite(in + 4, 1, dlen, fp) != dlen) {
                        perror("fwrite"); goto fail;
                    }
                    tftp_build_ack(last_ack, sizeof(last_ack), blk_wire);
                    expected32++;
                    pkts_in_window++;

                    if (dlen < TFTP_BLOCK_SIZE) {
                        /* Dernier bloc : ACK final et fin */
                        sendto(sock, last_ack, 4, 0, (struct sockaddr *)&peer, peerlen);
                        done = 1;
                        break;
                    }
                    /* Si on a recu tous les paquets de la fenetre, ACK */
                    if (pkts_in_window >= neg.windowsize) {
                        sendto(sock, last_ack, 4, 0, (struct sockaddr *)&peer, peerlen);
                        pkts_in_window = 0;
                        break; /* attendre la prochaine fenetre */
                    }
                    /* Sinon continuer a recevoir dans cette fenetre */
                } else if (cls == 0) {
                    /* Bloc duplique : re-ACK */
                    sendto(sock, last_ack, 4, 0, (struct sockaddr *)&peer, peerlen);
                }
                /* cls == -1 : hors ordre, ignorer */
            }
        }
    }

    fclose(fp);
    close(sock);
    return 0;

fail:
    if (fp)      fclose(fp);
    if (sock>=0) close(sock);
    return 1;
}

/* ══════════════════════════════════════════════════════
 *  TFTP PUT  (WRQ)
 *  Supporte : bigfile + windowsize
 * ══════════════════════════════════════════════════════ */
static int tftp_put(const char *server_ip, const char *local, const char *remote,
                    uint16_t port, int timeout_ms, const tftp_options_t *req_opts) {
    int   sock = -1;
    FILE *fp   = NULL;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    if (resolve_ipv4(server_ip, &srv, port) < 0) {
        fprintf(stderr, "Invalid server IPv4: %s\n", server_ip);
        close(sock); return 1;
    }

    fp = fopen(local, "rb");
    if (!fp) { perror("fopen"); close(sock); return 1; }

    /* Construire le WRQ */
    uint8_t out[TFTP_MAX_OPT_PKT];
    size_t  outlen;
    int using_opts = (req_opts && (req_opts->bigfile || req_opts->windowsize > 1));
    if (using_opts) {
        outlen = tftp_build_rrq_wrq_opts(out, sizeof(out), TFTP_OP_WRQ,
                                          remote, TFTP_MODE, req_opts);
    } else {
        outlen = tftp_build_rrq_wrq(out, sizeof(out), TFTP_OP_WRQ, remote, TFTP_MODE);
    }
    if (!outlen) { fprintf(stderr, "WRQ build failed\n"); goto fail; }

    if (sendto(sock, out, outlen, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("sendto(WRQ)"); goto fail;
    }

    tftp_options_t neg;
    tftp_options_init(&neg);

    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    uint8_t in[TFTP_MAX_PKT];
    ssize_t n;

    /* ─── Phase 1 : attente ACK(0) ou OACK ─── */
    {
        int retries = 0;
        for (;;) {
            int w = wait_readable(sock, timeout_ms);
            if (w < 0) { perror("select"); goto fail; }
            if (w == 0) {
                if (++retries >= TFTP_MAX_RETRIES) {
                    fprintf(stderr, "Timeout waiting for ACK0/OACK\n"); goto fail;
                }
                sendto(sock, out, outlen, 0, (struct sockaddr *)&srv, sizeof(srv));
                continue;
            }

            n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&peer, &peerlen);
            if (n < 4) continue;

            uint16_t op = tftp_rd_u16(in);
            if (op == TFTP_OP_ERROR) {
                fprintf(stderr, "TFTP ERROR %u: %s\n", tftp_rd_u16(in+2), (char*)(in+4));
                goto fail;
            }
            if (op == TFTP_OP_OACK) {
                if (tftp_parse_options(in, (size_t)n, 2, &neg) < 0) {
                    send_error_pkt(sock, &peer, peerlen, TFTP_ERR_ILLEGAL_OP, "Bad OACK");
                    goto fail;
                }
                fprintf(stderr, "[client] OACK accepted: bigfile=%d windowsize=%d\n",
                        neg.bigfile, neg.windowsize);
                break;
            }
            if (op == TFTP_OP_ACK && tftp_rd_u16(in + 2) == 0) {
                /* Serveur sans options : stop-and-wait classique */
                break;
            }
        }
    }

    /* ─── Phase 2 : envoi des blocs DATA (avec windowsize) ─── */
    {
        uint32_t block32 = 0;
        int      done    = 0;
        int      eof     = 0;

        /* Tampon de la fenetre : neg.windowsize paquets maximum */
        typedef struct { uint8_t pkt[TFTP_MAX_PKT]; size_t len; } slot_t;
        slot_t *window = (slot_t *)calloc((size_t)neg.windowsize, sizeof(slot_t));
        if (!window) { perror("calloc"); goto fail; }

        while (!done) {
            /* Remplir la fenetre depuis le fichier */
            int nslots = 0;
            for (int s = 0; s < neg.windowsize && !eof; s++) {
                uint8_t filebuf[TFTP_BLOCK_SIZE];
                size_t rd = fread(filebuf, 1, sizeof(filebuf), fp);
                if (ferror(fp)) { perror("fread"); free(window); goto fail; }
                block32++;
                size_t pktlen = tftp_build_data(window[s].pkt, TFTP_MAX_PKT,
                                                wire_block(block32), filebuf, rd);
                if (!pktlen) { fprintf(stderr, "DATA build failed\n");
                               free(window); goto fail; }
                window[s].len = pktlen;
                nslots++;
                if (rd < TFTP_BLOCK_SIZE) { eof = 1; }
            }

            /* Envoyer tous les paquets de la fenetre */
            for (int s = 0; s < nslots; s++) {
                if (sendto(sock, window[s].pkt, window[s].len, 0,
                           (struct sockaddr *)&peer, peerlen) < 0) {
                    perror("sendto(DATA)"); free(window); goto fail;
                }
            }

            /* Attendre l'ACK du dernier paquet de la fenetre */
            uint16_t expected_ack = wire_block(block32);
            int retries = 0;
            for (;;) {
                int w = wait_readable(sock, timeout_ms);
                if (w < 0) { perror("select"); free(window); goto fail; }
                if (w == 0) {
                    if (++retries >= TFTP_MAX_RETRIES) {
                        fprintf(stderr, "Timeout: too many retries\n");
                        free(window); goto fail;
                    }
                    /* Retransmettre toute la fenetre */
                    for (int s = 0; s < nslots; s++) {
                        sendto(sock, window[s].pkt, window[s].len, 0,
                               (struct sockaddr *)&peer, peerlen);
                    }
                    continue;
                }
                retries = 0;

                struct sockaddr_in ack_src;
                socklen_t ack_srclen = sizeof(ack_src);
                n = recvfrom(sock, in, sizeof(in), 0,
                             (struct sockaddr *)&ack_src, &ack_srclen);
                if (n < 4) continue;
/* Verification du TID du serveur sur chaque ACK recu (RFC 1350).
 * peer est fixe apres reception de ACK(0)/OACK lors du handshake. */
                
                if (ack_src.sin_addr.s_addr != peer.sin_addr.s_addr ||
                    ack_src.sin_port         != peer.sin_port) {
                    send_error_pkt(sock, &ack_src, ack_srclen,
                                   TFTP_ERR_UNKNOWN_TID, "Unknown TID");
                    continue;
                }

                uint16_t op = tftp_rd_u16(in);
                if (op == TFTP_OP_ERROR) {
                    fprintf(stderr, "TFTP ERROR %u: %s\n",
                            tftp_rd_u16(in+2), (char*)(in+4));
                    free(window); goto fail;
                }
                if (op != TFTP_OP_ACK) continue;

                uint16_t ackblk = tftp_rd_u16(in + 2);
                if (ackblk == expected_ack) break; /* fenetre acquittee */
                /* ACK partiel : possible avec windowsize > 1 en cas de perte */
            }

            if (eof) done = 1;
        }
        free(window);
    }

    fclose(fp);
    close(sock);
    return 0;

fail:
    if (fp)      fclose(fp);
    if (sock>=0) close(sock);
    return 1;
}

/* ══════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 5) { usage(argv[0]); return 1; }

    const char *cmd       = argv[1];
    const char *server_ip = argv[2];
    uint16_t    port      = 69;
    int         timeout_ms = 1000;
    tftp_options_t opts;
    tftp_options_init(&opts);

    if (strcmp(cmd, "get") == 0) {
        const char *remote = argv[3];
        const char *local  = argv[4];
        if (argc >= 6) port       = (uint16_t)atoi(argv[5]);
        if (argc >= 7) timeout_ms =            atoi(argv[6]);
        if (argc >= 8) {
            int w = atoi(argv[7]);
            if (w >= 1 && w <= TFTP_MAX_WINDOW) opts.windowsize = w;
        }
        if (argc >= 9) opts.bigfile = (atoi(argv[8]) != 0) ? 1 : 0;
        return tftp_get(server_ip, remote, local, port, timeout_ms, &opts);

    } else if (strcmp(cmd, "put") == 0) {
        const char *local  = argv[3];
        const char *remote = argv[4];
        if (argc >= 6) port       = (uint16_t)atoi(argv[5]);
        if (argc >= 7) timeout_ms =            atoi(argv[6]);
        if (argc >= 8) {
            int w = atoi(argv[7]);
            if (w >= 1 && w <= TFTP_MAX_WINDOW) opts.windowsize = w;
        }
        if (argc >= 9) opts.bigfile = (atoi(argv[8]) != 0) ? 1 : 0;
        return tftp_put(server_ip, local, remote, port, timeout_ms, &opts);

    } else {
        usage(argv[0]);
        return 1;
    }
}
