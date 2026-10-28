/* Lists DroidDeck's emulated /dev/video-enc0 when /dev is read, so FFmpeg's V4L2 M2M device scan finds it.
 * The device itself lives only in DroidDeck's preload (open/ioctl/mmap/poll), which has no directory entry. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DEVENC_NAME "video-enc0"
#define DEVENC_SLOTS 8

struct slot {
    DIR *dir;
    int seen, emitted;
    struct dirent64 ent;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot slots[DEVENC_SLOTS];

static int enabled(void) {
    const char *socket = getenv("BL_VENC_SOCKET");
    return socket != NULL && *socket != '\0';
}

static int is_dev(const char *path) {
    return path != NULL && (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0);
}

static struct slot *find(DIR *dir) {
    for (int i = 0; i < DEVENC_SLOTS; i++)
        if (slots[i].dir == dir) return &slots[i];
    return NULL;
}

DIR *opendir(const char *path) {
    static DIR *(*real)(const char *);
    if (real == NULL) real = dlsym(RTLD_NEXT, "opendir");
    DIR *dir = real(path);
    if (dir != NULL && is_dev(path) && enabled()) {
        pthread_mutex_lock(&lock);
        struct slot *s = find(NULL);
        if (s != NULL) memset(s, 0, sizeof *s), s->dir = dir;
        pthread_mutex_unlock(&lock);
    }
    return dir;
}

/* On aarch64 glibc struct dirent and struct dirent64 share one layout, so both entry points return the same entry. */
static struct dirent64 *next_entry(DIR *dir, struct dirent64 *(*real)(DIR *)) {
    struct dirent64 *e = real(dir);
    pthread_mutex_lock(&lock);
    struct slot *s = find(dir);
    if (s != NULL) {
        if (e != NULL && strcmp(e->d_name, DEVENC_NAME) == 0) s->seen = 1;
        if (e == NULL && !s->seen && !s->emitted) {
            s->emitted = 1;
            memset(&s->ent, 0, sizeof s->ent);
            s->ent.d_ino = 1;
            s->ent.d_type = DT_CHR;
            s->ent.d_reclen = sizeof s->ent;
            strcpy(s->ent.d_name, DEVENC_NAME);
            e = &s->ent;
        }
    }
    pthread_mutex_unlock(&lock);
    return e;
}

struct dirent64 *readdir64(DIR *dir) {
    static struct dirent64 *(*real)(DIR *);
    if (real == NULL) real = dlsym(RTLD_NEXT, "readdir64");
    return next_entry(dir, real);
}

struct dirent *readdir(DIR *dir) {
    static struct dirent64 *(*real)(DIR *);
    if (real == NULL) real = (struct dirent64 * (*)(DIR *)) dlsym(RTLD_NEXT, "readdir");
    return (struct dirent *)next_entry(dir, real);
}

int closedir(DIR *dir) {
    static int (*real)(DIR *);
    if (real == NULL) real = dlsym(RTLD_NEXT, "closedir");
    pthread_mutex_lock(&lock);
    struct slot *s = find(dir);
    if (s != NULL) s->dir = NULL;
    pthread_mutex_unlock(&lock);
    return real(dir);
}
