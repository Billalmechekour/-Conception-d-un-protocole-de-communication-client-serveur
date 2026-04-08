#include "file_lock.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct lock_entry {
    char name[256];
    pthread_rwlock_t rw;
    int refcnt;
    struct lock_entry *next;
} lock_entry_t;

struct file_lock_handle {
    lock_entry_t *e;
    file_lock_mode_t mode;
};

static pthread_mutex_t g_table_mtx = PTHREAD_MUTEX_INITIALIZER;
static lock_entry_t *g_head = NULL;

static lock_entry_t* find_or_create_entry(const char *filename) {
    lock_entry_t *cur = g_head;
    while (cur) {
        if (strcmp(cur->name, filename) == 0) return cur;
        cur = cur->next;
    }
    lock_entry_t *ne = (lock_entry_t*)calloc(1, sizeof(*ne));
    if (!ne) return NULL;
    strncpy(ne->name, filename, sizeof(ne->name) - 1);
    if (pthread_rwlock_init(&ne->rw, NULL) != 0) {
        free(ne);
        return NULL;
    }
    ne->refcnt = 0;
    ne->next = g_head;
    g_head = ne;
    return ne;
}

file_lock_handle_t* file_lock_acquire(const char *filename, file_lock_mode_t mode) {
    if (!filename || !*filename) return NULL;

    file_lock_handle_t *h = (file_lock_handle_t*)calloc(1, sizeof(*h));
    if (!h) return NULL;

    pthread_mutex_lock(&g_table_mtx);
    lock_entry_t *e = find_or_create_entry(filename);
    if (!e) {
        pthread_mutex_unlock(&g_table_mtx);
        free(h);
        return NULL;
    }
    e->refcnt++;
    pthread_mutex_unlock(&g_table_mtx);

    int rc;
    if (mode == FILE_LOCK_WRITE) rc = pthread_rwlock_wrlock(&e->rw);
    else                        rc = pthread_rwlock_rdlock(&e->rw);

    if (rc != 0) {
        pthread_mutex_lock(&g_table_mtx);
        e->refcnt--;
        pthread_mutex_unlock(&g_table_mtx);
        free(h);
        errno = rc;
        return NULL;
    }

    h->e = e;
    h->mode = mode;
    return h;
}

void file_lock_release(file_lock_handle_t *h) {
    if (!h || !h->e) return;

    pthread_rwlock_unlock(&h->e->rw);

    pthread_mutex_lock(&g_table_mtx);
    h->e->refcnt--;
    if (h->e->refcnt <= 0) {
        lock_entry_t **pp = &g_head;
        while (*pp) {
            if (*pp == h->e) {
                *pp = h->e->next;
                pthread_rwlock_destroy(&h->e->rw);
                free(h->e);
                break;
            }
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_table_mtx);

    free(h);
}

void file_lock_table_destroy(void) {
    pthread_mutex_lock(&g_table_mtx);
    lock_entry_t *cur = g_head;
    g_head = NULL;
    pthread_mutex_unlock(&g_table_mtx);

    while (cur) {
        lock_entry_t *n = cur->next;
        pthread_rwlock_destroy(&cur->rw);
        free(cur);
        cur = n;
    }
}
