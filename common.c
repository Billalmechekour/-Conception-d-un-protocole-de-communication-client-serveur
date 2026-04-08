#include "common.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

/* ══════════════════════════════════════════════════════
 *  Fonctions de base (RFC 1350) — inchangees depuis etape 1
 * ══════════════════════════════════════════════════════ */

uint16_t tftp_rd_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

void tftp_wr_u16(uint8_t *p, uint16_t v) {
    uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

size_t tftp_build_rrq_wrq(uint8_t *buf, size_t buflen, uint16_t opcode,
                           const char *filename, const char *mode) {
    if (!buf || !filename || !mode) return 0;
    size_t fn   = strlen(filename);
    size_t md   = strlen(mode);
    size_t need = 2 + fn + 1 + md + 1;
    if (need > buflen) return 0;
    tftp_wr_u16(buf, opcode);
    memcpy(buf + 2, filename, fn);
    buf[2 + fn] = '\0';
    memcpy(buf + 2 + fn + 1, mode, md);
    buf[2 + fn + 1 + md] = '\0';
    return need;
}

size_t tftp_build_ack(uint8_t *buf, size_t buflen, uint16_t block) {
    if (!buf || buflen < 4) return 0;
    tftp_wr_u16(buf, TFTP_OP_ACK);
    tftp_wr_u16(buf + 2, block);
    return 4;
}

size_t tftp_build_data(uint8_t *buf, size_t buflen, uint16_t block,
                       const uint8_t *data, size_t data_len) {
    if (!buf || buflen < 4) return 0;
    if (data_len > TFTP_BLOCK_SIZE) return 0;
    if (4 + data_len > buflen) return 0;
    tftp_wr_u16(buf, TFTP_OP_DATA);
    tftp_wr_u16(buf + 2, block);
    if (data_len > 0) memcpy(buf + 4, data, data_len);
    return 4 + data_len;
}

size_t tftp_build_error(uint8_t *buf, size_t buflen, uint16_t errcode,
                        const char *errmsg) {
    if (!buf || !errmsg) return 0;
    size_t em   = strlen(errmsg);
    size_t need = 4 + em + 1;
    if (need > buflen) return 0;
    tftp_wr_u16(buf, TFTP_OP_ERROR);
    tftp_wr_u16(buf + 2, errcode);
    memcpy(buf + 4, errmsg, em);
    buf[4 + em] = '\0';
    return need;
}

int wait_readable(int fd, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r;
    do { r = select(fd + 1, &rfds, NULL, NULL, &tv); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;
}

static int has_slash(const char *s) {
    for (; *s; s++) if (*s == '/' || *s == '\\') return 1;
    return 0;
}

int filename_is_safe(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (has_slash(name)) return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

/* ══════════════════════════════════════════════════════
 *  Etapes 4 & 5 : negotiation d'options (RFC 2347)
 * ══════════════════════════════════════════════════════ */

/*
 * Ajoute une paire opt\0valeur\0 dans buf a partir de *pos.
 * Retourne 0 si succes, -1 si depassement de buflen.
 */
static int append_opt(uint8_t *buf, size_t buflen, size_t *pos,
                      const char *opt, const char *val) {
    size_t ol = strlen(opt);
    size_t vl = strlen(val);
    if (*pos + ol + 1 + vl + 1 > buflen) return -1;
    memcpy(buf + *pos, opt, ol);
    *pos += ol;
    buf[(*pos)++] = '\0';
    memcpy(buf + *pos, val, vl);
    *pos += vl;
    buf[(*pos)++] = '\0';
    return 0;
}

/*
 * Construit un RRQ/WRQ avec les options activees dans *opts.
 *
 * Format (RFC 2347) :
 *   opcode(2) | filename\0 | mode\0 | opt1\0 | val1\0 | ...
 */
size_t tftp_build_rrq_wrq_opts(uint8_t *buf, size_t buflen, uint16_t opcode,
                                const char *filename, const char *mode,
                                const tftp_options_t *opts) {
    /* Base : RRQ/WRQ sans options */
    size_t pos = tftp_build_rrq_wrq(buf, buflen, opcode, filename, mode);
    if (!pos) return 0;
    if (!opts) return pos;

    /* Ajout de l'option bigfile si activee (etape 4) */
    if (opts->bigfile) {
        if (append_opt(buf, buflen, &pos, TFTP_OPT_BIGFILE, "1") < 0) return 0;
    }

    /* Ajout de l'option windowsize si > 1 (etape 5) */
    if (opts->windowsize > 1) {
        char val[16];
        snprintf(val, sizeof(val), "%d", opts->windowsize);
        if (append_opt(buf, buflen, &pos, TFTP_OPT_WINDOWSIZE, val) < 0) return 0;
    }

    return pos;
}

/*
 * Construit un paquet OACK (opcode 6, RFC 2347).
 * N'inclut que les options dont le flag correspondant est != 0.
 *
 * Format : opcode(2) | opt1\0 | val1\0 | ...
 */
size_t tftp_build_oack(uint8_t *buf, size_t buflen,
                        const tftp_options_t *confirmed,
                        int has_bigfile, int has_windowsize) {
    if (!buf || !confirmed || buflen < 2) return 0;
    tftp_wr_u16(buf, TFTP_OP_OACK);
    size_t pos = 2;

    if (has_bigfile && confirmed->bigfile) {
        if (append_opt(buf, buflen, &pos, TFTP_OPT_BIGFILE, "1") < 0) return 0;
    }
    if (has_windowsize && confirmed->windowsize > 1) {
        char val[16];
        snprintf(val, sizeof(val), "%d", confirmed->windowsize);
        if (append_opt(buf, buflen, &pos, TFTP_OPT_WINDOWSIZE, val) < 0) return 0;
    }
    return pos;
}

/*
 * Analyse les paires cle\0valeur\0 dans buf[offset..len-1].
 * Utilisee pour :
 *   - RRQ/WRQ  : offset = position apres le champ mode
 *   - OACK     : offset = 2 (apres l'opcode)
 *
 * Reconnait les options : "bigfile" et "windowsize".
 * Retourne 0 si succes, -1 si paquet invalide.
 */
int tftp_parse_options(const uint8_t *buf, size_t len, size_t offset,
                       tftp_options_t *opts) {
    if (!buf || !opts || offset > len) return -1;
    tftp_options_init(opts);

    size_t i = offset;
    while (i < len) {
        /* Lire la cle */
        const char *key = (const char *)(buf + i);
        size_t key_start = i;
        while (i < len && buf[i] != '\0') i++;
        if (i >= len) return -1; /* pas de '\0' terminateur */
        i++; /* sauter le '\0' */
        if (i >= len && key_start == i - 1) break; /* cle vide en fin */

        /* Lire la valeur */
        const char *val = (const char *)(buf + i);
        while (i < len && buf[i] != '\0') i++;
        if (buf[i] != '\0') return -1;
        i++;

        /* Identifier l'option */
        if (strcasecmp(key, TFTP_OPT_BIGFILE) == 0) {
            /* Correction critique (RFC 2347) : seule la valeur "1" est valide.
             * Toute autre valeur est une erreur protocolaire -> retour -1.
             * L'appelant doit envoyer ERROR(0, "Invalid bigfile option value"). */
            if (strcmp(val, "1") != 0) return -1;
            opts->bigfile = 1;
        } else if (strcasecmp(key, TFTP_OPT_WINDOWSIZE) == 0) {
            int w = atoi(val);
            if (w >= 1 && w <= TFTP_MAX_WINDOW)
                opts->windowsize = w;
        }
        /* Options inconnues : ignorees (RFC 2347, section 3) */
    }
    return 0;
}
