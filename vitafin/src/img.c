#include "img.h"
#include "jf.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Posters load on two background threads, at the smallest size Jellyfin will give us,
   so scrolling never waits on the network or on JPEG decoding. */

#define SLOTS 80
enum { FREE, QUEUED, LOADING, READY, FAILED };

typedef struct {
  char id[JF_ID];
  int maxw, state;
  uint32_t used;
  vita2d_texture *tex;
} slot;

static slot slots[SLOTS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t tick;

static void *worker(void *arg) {
  (void)arg;
  for (;;) {
    int pick = -1;
    pthread_mutex_lock(&mu);
    uint32_t best = 0;
    for (int i = 0; i < SLOTS; i++)
      if (slots[i].state == QUEUED && slots[i].used >= best) {
        best = slots[i].used; /* newest request first: what's on screen now */
        pick = i;
      }
    if (pick >= 0) slots[pick].state = LOADING;
    pthread_mutex_unlock(&mu);

    if (pick < 0) {
      usleep(30 * 1000);
      continue;
    }

    char url[512];
    uint8_t *buf = NULL;
    size_t len = 0;
    jf_url_image(slots[pick].id, slots[pick].maxw, url, sizeof url);
    vita2d_texture *t = NULL;
    if (jf_http_get(url, &buf, &len) == 0) {
      t = vita2d_load_JPEG_buffer(buf, len);
      free(buf);
    }
    pthread_mutex_lock(&mu);
    slots[pick].tex = t;
    slots[pick].state = t ? READY : FAILED;
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

void img_init(void) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 * 1024);
  for (int i = 0; i < 2; i++) {
    pthread_t t;
    pthread_create(&t, &a, worker, NULL);
    pthread_detach(t);
  }
}

vita2d_texture *img_get(const char *id, int maxw) {
  pthread_mutex_lock(&mu);
  tick++;
  int free_i = -1, lru = -1;
  uint32_t lru_used = 0xFFFFFFFF;
  for (int i = 0; i < SLOTS; i++) {
    if (slots[i].state != FREE && !strcmp(slots[i].id, id)) {
      slots[i].used = tick;
      vita2d_texture *t = slots[i].state == READY ? slots[i].tex : NULL;
      pthread_mutex_unlock(&mu);
      return t;
    }
    if (slots[i].state == FREE && free_i < 0) free_i = i;
    if ((slots[i].state == READY || slots[i].state == FAILED) && slots[i].used < lru_used) {
      lru_used = slots[i].used;
      lru = i;
    }
  }
  int use = free_i;
  vita2d_texture *old = NULL;
  if (use < 0 && lru >= 0) {
    use = lru;
    old = slots[use].tex;
    slots[use].tex = NULL;
  }
  if (use >= 0) {
    snprintf(slots[use].id, sizeof slots[use].id, "%s", id);
    slots[use].maxw = maxw;
    slots[use].state = QUEUED;
    slots[use].used = tick;
  }
  pthread_mutex_unlock(&mu);
  if (old) {
    vita2d_wait_rendering_done(); /* GPU may still be sampling the evicted texture */
    vita2d_free_texture(old);
  }
  return NULL;
}
