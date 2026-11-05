#pragma once
#include <stddef.h>
#include <stdint.h>

#define JF_ID 64

typedef struct {
  char id[JF_ID];
  char name[160];
  char type[24];     /* Movie, Series, Season, Episode, MusicAlbum, Audio, ... */
  char series[120];
  int year, index, parent_index, played;
  int64_t runtime;   /* ticks */
  int64_t resume;    /* ticks */
} jf_item;

typedef struct {
  jf_item *v;
  int n, total;
} jf_list;

typedef struct {
  char overview[1200];
  char source_id[JF_ID];
  char container[24], vcodec[24], acodec[24];
  int width, height;
  int nsubs;
  struct {
    int index;
    char lang[12], title[64], codec[16];
  } subs[16];
} jf_detail;

int jf_init(void);                 /* 0 = server configured */
int jf_have_token(void);
int jf_server_set(void);
void jf_logout(void);              /* forget the saved sign-in */
int jf_has_password_login(void);
int jf_login_password(void);       /* 0 ok */
int jf_quick_start(char *code, size_t cn, char *secret, size_t sn); /* 0 ok */
int jf_quick_poll(const char *secret);                              /* 1 = signed in */

int jf_views(jf_list *out);
int jf_resume(jf_list *out);
int jf_children(const char *parent, const char *types, int start, int limit, jf_list *out);
int jf_detail_get(const char *id, jf_detail *d);
void jf_list_free(jf_list *l);

void jf_url_image(const char *id, int maxw, char *out, size_t n);
void jf_url_video(const char *id, const jf_detail *d, char *out, size_t n);
void jf_url_audio(const char *id, char *out, size_t n);
void jf_url_sub(const char *id, const jf_detail *d, int idx, char *out, size_t n);

int jf_http_get(const char *url, uint8_t **buf, size_t *len); /* caller frees */
int jf_http_range(const char *url, uint64_t off, uint32_t len, uint8_t *dst);
int64_t jf_http_size(const char *url);

void jf_report(const char *id, const char *source_id, int64_t pos_ticks, int state, int paused);
