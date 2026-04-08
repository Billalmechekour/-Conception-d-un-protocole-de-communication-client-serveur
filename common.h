#ifndef TFTP_COMMON_H
#define TFTP_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════
 *  OpCodes  RFC 1350  +  RFC 2347
 * ═══════════════════════════════════════════════════ */
#define TFTP_OP_RRQ   1
#define TFTP_OP_WRQ   2
#define TFTP_OP_DATA  3
#define TFTP_OP_ACK   4
#define TFTP_OP_ERROR 5
#define TFTP_OP_OACK  6   /* RFC 2347 - Option Acknowledgment */

/* ═══════════════════════════════════════════════════
 *  Tailles et limites
 * ═══════════════════════════════════════════════════ */
#define TFTP_BLOCK_SIZE  512
#define TFTP_MAX_PKT     (4 + TFTP_BLOCK_SIZE)
#define TFTP_MAX_OPT_PKT 1024
#define TFTP_MAX_RETRIES 8
#define TFTP_MAX_WINDOW  64

#define TFTP_MODE "octet"

/* ═══════════════════════════════════════════════════
 *  Codes d'erreur  RFC 1350
 * ═══════════════════════════════════════════════════ */
#define TFTP_ERR_UNDEF          0
#define TFTP_ERR_FILE_NOT_FOUND 1
#define TFTP_ERR_ACCESS_VIOL    2
#define TFTP_ERR_DISK_FULL      3
#define TFTP_ERR_ILLEGAL_OP     4
#define TFTP_ERR_UNKNOWN_TID    5
#define TFTP_ERR_FILE_EXISTS    6
#define TFTP_ERR_NO_SUCH_USER   7

/* ═══════════════════════════════════════════════════
 *  Noms des options (RFC 2347)
 * ═══════════════════════════════════════════════════ */
#define TFTP_OPT_BIGFILE    "bigfile"
#define TFTP_OPT_WINDOWSIZE "windowsize"

/* ═══════════════════════════════════════════════════
 *  Options negociees
 * ═══════════════════════════════════════════════════ */
typedef struct {
    int bigfile;    /* 0=off ; 1=rollover 32 bits active (etape 4) */
    int windowsize; /* 1=stop-and-wait ; N>1=fenetre glissante (etape 5) */
} tftp_options_t;

static inline void tftp_options_init(tftp_options_t *o) {
    if (!o) return;
    o->bigfile    = 0;
    o->windowsize = 1;
}

/* ═══════════════════════════════════════════════════
 *  Fonctions RFC 1350 (inchangees)
 * ═══════════════════════════════════════════════════ */
uint16_t tftp_rd_u16(const uint8_t *p);
void     tftp_wr_u16(uint8_t *p, uint16_t v);

size_t tftp_build_rrq_wrq(uint8_t *buf, size_t buflen, uint16_t opcode,
                           const char *filename, const char *mode);
size_t tftp_build_ack   (uint8_t *buf, size_t buflen, uint16_t block);
size_t tftp_build_data  (uint8_t *buf, size_t buflen, uint16_t block,
                          const uint8_t *data, size_t data_len);
size_t tftp_build_error (uint8_t *buf, size_t buflen, uint16_t errcode,
                          const char *errmsg);

/* ═══════════════════════════════════════════════════
 *  Etapes 4 & 5 : options RFC 2347
 * ═══════════════════════════════════════════════════ */
size_t tftp_build_rrq_wrq_opts(uint8_t *buf, size_t buflen, uint16_t opcode,
                                const char *filename, const char *mode,
                                const tftp_options_t *opts);

size_t tftp_build_oack(uint8_t *buf, size_t buflen,
                        const tftp_options_t *confirmed,
                        int has_bigfile, int has_windowsize);

int tftp_parse_options(const uint8_t *buf, size_t len, size_t offset,
                       tftp_options_t *opts);

/* ═══════════════════════════════════════════════════
 *  Utilitaires
 * ═══════════════════════════════════════════════════ */
int wait_readable(int fd, int timeout_ms);
int filename_is_safe(const char *name);

#ifdef __cplusplus
}
#endif
#endif
