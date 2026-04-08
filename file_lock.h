#ifndef FILE_LOCK_H
#define FILE_LOCK_H

#include <pthread.h>

typedef enum {
    FILE_LOCK_READ  = 0,
    FILE_LOCK_WRITE = 1
} file_lock_mode_t;

typedef struct file_lock_handle file_lock_handle_t;

/* Acquiert un verrou par nom de fichier (RW-lock):
 * - READ  : plusieurs lecteurs simultanés autorisés
 * - WRITE : exclusif (bloque lecteurs + autres writers)
 * Le verrou est conservé pendant tout le transfert (cohérence).
 */
file_lock_handle_t* file_lock_acquire(const char *filename, file_lock_mode_t mode);

/* Libère le verrou acquis */
void file_lock_release(file_lock_handle_t *h);

/* Optionnel (fin de programme) */
void file_lock_table_destroy(void);

#endif
