#include "jf.h"
#include "util.h"

#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* libcurl was built with zstd support; we never use it, so provide empty stand-ins
   instead of linking the real library. */
void *ZSTD_createDStream(void) { return 0; }
size_t ZSTD_freeDStream(void *s) { (void)s; return 0; }
size_t ZSTD_decompressStream(void *s, void *o, void *i) { (void)s; (void)o; (void)i; return (size_t)-1; }
unsigned ZSTD_isError(size_t c) { return c > (size_t)-120; }
static char server[256], token[160], uid[64], cfg_user[96], cfg_pass[96];

struct mem {
  uint8_t *p;
  size_t n, cap;
};

static size_t wcb(void *d, size_t s, size_t n, void *u) {
  struct mem *m = u;
  size_t k = s * n;
  if (m->n + k + 1 > m->cap) {
    size_t c = m->cap ? m->cap * 2 : 16384;
    while (c < m->n + k + 1) c *= 2;
    uint8_t *np = realloc(m->p, c);
    if (!np) return 0;
    m->p = np;
    m->cap = c;
  }
  memcpy(m->p + m->n, d, k);
  m->n += k;
  m->p[m->n] = 0;
  return k;
}

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1);
  if (!b) {
    fclose(f);
    return NULL;
  }
  size_t r = fread(b, 1, n, f);
  b[r] = 0;
  fclose(f);
  return b;
}

static void jstr(json_t *o, const char *k, char *dst, size_t n) {
  json_t *v = json_object_get(o, k);
  snprintf(dst, n, "%s", json_is_string(v) ? json_string_value(v) : "");
}

static int64_t jint(json_t *o, const char *k) {
  json_t *v = json_object_get(o, k);
  return json_is_number(v) ? (int64_t)json_number_value(v) : 0;
}

static void save_auth(void) {
  json_t *o = json_pack("{s:s,s:s}", "token", token, "user_id", uid);
  char *s = json_dumps(o, JSON_INDENT(2));
  FILE *f = fopen(DATA_DIR "/auth.json", "wb");
  if (f && s) {
    fputs(s, f);
    fclose(f);
  }
  free(s);
  json_decref(o);
}

int jf_init(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  char *c = slurp(DATA_DIR "/config.json");
  if (c) {
    json_t *o = json_loads(c, 0, NULL);
    if (o) {
      jstr(o, "server", server, sizeof server);
      jstr(o, "username", cfg_user, sizeof cfg_user);
      jstr(o, "password", cfg_pass, sizeof cfg_pass);
      json_decref(o);
    }
    free(c);
  }
  size_t l = strlen(server);
  while (l && server[l - 1] == '/') server[--l] = 0;
  c = slurp(DATA_DIR "/auth.json");
  if (c) {
    json_t *o = json_loads(c, 0, NULL);
    if (o) {
      jstr(o, "token", token, sizeof token);
      jstr(o, "user_id", uid, sizeof uid);
      json_decref(o);
    }
    free(c);
  }
  return server[0] ? 0 : -1;
}

int jf_have_token(void) { return token[0] && uid[0]; }
int jf_server_set(void) { return server[0] != 0; }
void jf_logout(void) {
  token[0] = 0;
  uid[0] = 0;
  remove(DATA_DIR "/auth.json");
}
int jf_has_password_login(void) { return cfg_user[0] != 0; }

static long request(const char *method, const char *path, const char *body, struct mem *m) {
  CURL *c = curl_easy_init();
  if (!c) return 0;
  char url[1536];
  if (!strncmp(path, "http", 4)) snprintf(url, sizeof url, "%s", path);
  else snprintf(url, sizeof url, "%s%s", server, path);

  char auth[512];
  snprintf(auth, sizeof auth,
           "Authorization: MediaBrowser Client=\"VitaFin\", Device=\"PS Vita\", "
           "DeviceId=\"vitafin-vita\", Version=\"0.1.0\"%s%s%s",
           token[0] ? ", Token=\"" : "", token, token[0] ? "\"" : "");
  struct curl_slist *h = curl_slist_append(NULL, auth);
  if (body) h = curl_slist_append(h, "Content-Type: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");
  if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);

  long code = 0;
  if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h);
  curl_easy_cleanup(c);
  return code;
}

static json_t *api(const char *method, const char *path, const char *body) {
  struct mem m = {0};
  long code = request(method, path, body, &m);
  json_t *r = NULL;
  if (code >= 200 && code < 300 && m.p) r = json_loads((char *)m.p, 0, NULL);
  free(m.p);
  return r;
}

int jf_http_get(const char *url, uint8_t **buf, size_t *len) {
  struct mem m = {0};
  long code = request("GET", url, NULL, &m);
  if (code < 200 || code >= 300) {
    free(m.p);
    return -1;
  }
  *buf = m.p;
  *len = m.n;
  return 0;
}

/* ---- ranged reads for the video player (shared keep-alive handle) ---- */
struct fixed {
  uint8_t *p;
  size_t cap, n;
};

static size_t fcb(void *d, size_t s, size_t n, void *u) {
  struct fixed *f = u;
  size_t k = s * n;
  size_t room = f->cap - f->n;
  if (k > room) k = room;
  memcpy(f->p + f->n, d, k);
  f->n += k;
  return s * n;
}

static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
static CURL *rh;

int jf_http_range(const char *url, uint64_t off, uint32_t len, uint8_t *dst) {
  if (!len) return 0;
  pthread_mutex_lock(&rmu);
  if (!rh) rh = curl_easy_init();
  else curl_easy_reset(rh);
  char range[64];
  snprintf(range, sizeof range, "%llu-%llu", (unsigned long long)off,
           (unsigned long long)(off + len - 1));
  struct fixed f = {dst, len, 0};
  curl_easy_setopt(rh, CURLOPT_URL, url);
  curl_easy_setopt(rh, CURLOPT_RANGE, range);
  curl_easy_setopt(rh, CURLOPT_WRITEFUNCTION, fcb);
  curl_easy_setopt(rh, CURLOPT_WRITEDATA, &f);
  curl_easy_setopt(rh, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(rh, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(rh, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(rh, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(rh, CURLOPT_FOLLOWLOCATION, 1L);
  CURLcode rc = curl_easy_perform(rh);
  pthread_mutex_unlock(&rmu);
  return rc == CURLE_OK ? (int)f.n : -1;
}

int64_t jf_http_size(const char *url) {
  CURL *c = curl_easy_init();
  if (!c) return -1;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  int64_t size = -1;
  if (curl_easy_perform(c) == CURLE_OK) {
    curl_off_t cl = -1;
    curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    size = cl;
  }
  curl_easy_cleanup(c);
  return size;
}

/* ---- auth ---- */
static int take_auth(json_t *r) {
  if (!r) return -1;
  jstr(r, "AccessToken", token, sizeof token);
  json_t *u = json_object_get(r, "User");
  if (u) jstr(u, "Id", uid, sizeof uid);
  json_decref(r);
  if (!token[0] || !uid[0]) return -1;
  save_auth();
  return 0;
}

int jf_login_password(void) {
  json_t *b = json_pack("{s:s,s:s}", "Username", cfg_user, "Pw", cfg_pass);
  char *s = json_dumps(b, 0);
  json_decref(b);
  token[0] = 0;
  json_t *r = api("POST", "/Users/AuthenticateByName", s);
  free(s);
  return take_auth(r);
}

int jf_quick_start(char *code, size_t cn, char *secret, size_t sn) {
  token[0] = 0;
  json_t *r = api("POST", "/QuickConnect/Initiate", NULL);
  if (!r) return -1;
  jstr(r, "Code", code, cn);
  jstr(r, "Secret", secret, sn);
  json_decref(r);
  return code[0] && secret[0] ? 0 : -1;
}

int jf_quick_poll(const char *secret) {
  char path[256];
  snprintf(path, sizeof path, "/QuickConnect/Connect?secret=%s", secret);
  json_t *r = api("GET", path, NULL);
  if (!r) return 0;
  int ok = json_is_true(json_object_get(r, "Authenticated"));
  json_decref(r);
  if (!ok) return 0;
  json_t *b = json_pack("{s:s}", "Secret", secret);
  char *s = json_dumps(b, 0);
  json_decref(b);
  r = api("POST", "/Users/AuthenticateWithQuickConnect", s);
  free(s);
  return take_auth(r) == 0;
}

/* ---- item lists ---- */
static void fill_item(jf_item *it, json_t *o) {
  memset(it, 0, sizeof *it);
  jstr(o, "Id", it->id, sizeof it->id);
  jstr(o, "Name", it->name, sizeof it->name);
  jstr(o, "Type", it->type, sizeof it->type);
  jstr(o, "SeriesName", it->series, sizeof it->series);
  it->year = (int)jint(o, "ProductionYear");
  it->index = (int)jint(o, "IndexNumber");
  it->parent_index = (int)jint(o, "ParentIndexNumber");
  it->runtime = jint(o, "RunTimeTicks");
  json_t *ud = json_object_get(o, "UserData");
  if (ud) {
    it->played = json_is_true(json_object_get(ud, "Played"));
    it->resume = jint(ud, "PlaybackPositionTicks");
  }
}

static int take_list(json_t *r, jf_list *out) {
  memset(out, 0, sizeof *out);
  if (!r) return -1;
  json_t *a = json_is_array(r) ? r : json_object_get(r, "Items");
  size_t n = json_is_array(a) ? json_array_size(a) : 0;
  out->v = calloc(n ? n : 1, sizeof(jf_item));
  if (!out->v) {
    json_decref(r);
    return -1;
  }
  for (size_t i = 0; i < n; i++) fill_item(&out->v[i], json_array_get(a, i));
  out->n = (int)n;
  out->total = json_is_object(r) ? (int)jint(r, "TotalRecordCount") : (int)n;
  json_decref(r);
  return 0;
}

void jf_list_free(jf_list *l) {
  free(l->v);
  memset(l, 0, sizeof *l);
}

int jf_views(jf_list *out) {
  char p[256];
  snprintf(p, sizeof p, "/UserViews?userId=%s", uid);
  return take_list(api("GET", p, NULL), out);
}

int jf_resume(jf_list *out) {
  char p[384];
  snprintf(p, sizeof p,
           "/Users/%s/Items/Resume?Limit=24&MediaTypes=Video&Fields=ProductionYear", uid);
  return take_list(api("GET", p, NULL), out);
}

int jf_children(const char *parent, const char *types, int start, int limit, jf_list *out) {
  char p[768];
  int typed = types && *types;
  snprintf(p, sizeof p,
           "/Users/%s/Items?ParentId=%s&SortBy=%s&SortOrder=Ascending"
           "&Fields=ProductionYear&StartIndex=%d&Limit=%d%s%s%s",
           uid, parent,
           typed ? "SortName" : "ParentIndexNumber,IndexNumber,SortName", start, limit,
           typed ? "&Recursive=true&IncludeItemTypes=" : "", typed ? types : "", "");
  return take_list(api("GET", p, NULL), out);
}

int jf_detail_get(const char *id, jf_detail *d) {
  memset(d, 0, sizeof *d);
  char p[256];
  snprintf(p, sizeof p, "/Users/%s/Items/%s", uid, id);
  json_t *r = api("GET", p, NULL);
  if (!r) return -1;
  jstr(r, "Overview", d->overview, sizeof d->overview);
  json_t *ms = json_array_get(json_object_get(r, "MediaSources"), 0);
  if (ms) {
    jstr(ms, "Id", d->source_id, sizeof d->source_id);
    jstr(ms, "Container", d->container, sizeof d->container);
    json_t *st = json_object_get(ms, "MediaStreams");
    size_t i;
    json_t *s;
    json_array_foreach(st, i, s) {
      char type[16];
      jstr(s, "Type", type, sizeof type);
      if (!strcmp(type, "Video") && !d->vcodec[0]) {
        jstr(s, "Codec", d->vcodec, sizeof d->vcodec);
        d->width = (int)jint(s, "Width");
        d->height = (int)jint(s, "Height");
      } else if (!strcmp(type, "Audio") && !d->acodec[0]) {
        jstr(s, "Codec", d->acodec, sizeof d->acodec);
      } else if (!strcmp(type, "Subtitle") && d->nsubs < 16) {
        int k = d->nsubs++;
        d->subs[k].index = (int)jint(s, "Index");
        jstr(s, "Language", d->subs[k].lang, sizeof d->subs[k].lang);
        jstr(s, "Title", d->subs[k].title, sizeof d->subs[k].title);
        jstr(s, "Codec", d->subs[k].codec, sizeof d->subs[k].codec);
      }
    }
  }
  json_decref(r);
  return 0;
}

/* ---- URLs ---- */
void jf_url_image(const char *id, int maxw, char *out, size_t n) {
  snprintf(out, n, "%s/Items/%s/Images/Primary?maxWidth=%d&quality=70&format=jpg", server, id,
           maxw);
}

void jf_url_video(const char *id, const jf_detail *d, char *out, size_t n) {
  /* Direct play only when the Vita can decode it as-is: MP4/M4V, H.264, <=960 wide,
     AAC or MP3 audio. Everything else asks Jellyfin to transcode to that. */
  int direct = (strstr(d->container, "mp4") || strstr(d->container, "m4v")) &&
               !strcmp(d->vcodec, "h264") && d->width <= 960 &&
               (!strcmp(d->acodec, "aac") || !strcmp(d->acodec, "mp3"));
  if (direct)
    snprintf(out, n, "%s/Videos/%s/stream?static=true&MediaSourceId=%s&api_key=%s", server, id,
             d->source_id, token);
  else
    snprintf(out, n,
             "%s/Videos/%s/stream.mp4?MediaSourceId=%s&VideoCodec=h264&AudioCodec=aac"
             "&MaxWidth=960&MaxHeight=544&VideoBitRate=1800000&AudioBitRate=128000"
             "&MaxAudioChannels=2&api_key=%s",
             server, id, d->source_id, token);
}

void jf_url_audio(const char *id, char *out, size_t n) {
  snprintf(out, n, "%s/Audio/%s/stream?static=true&api_key=%s", server, id, token);
}

void jf_url_sub(const char *id, const jf_detail *d, int idx, char *out, size_t n) {
  snprintf(out, n, "%s/Videos/%s/%s/Subtitles/%d/0/Stream.srt?api_key=%s", server, id,
           d->source_id, idx, token);
}

void jf_report(const char *id, const char *source_id, int64_t pos, int state, int paused) {
  const char *path = state == 0   ? "/Sessions/Playing"
                     : state == 1 ? "/Sessions/Playing/Progress"
                                  : "/Sessions/Playing/Stopped";
  json_t *b = json_pack("{s:s,s:s,s:I,s:b}", "ItemId", id, "MediaSourceId", source_id,
                        "PositionTicks", (json_int_t)pos, "IsPaused", paused);
  char *s = json_dumps(b, 0);
  json_decref(b);
  struct mem m = {0};
  request("POST", path, s, &m);
  free(m.p);
  free(s);
}
