#include "ui.h"
#include "img.h"
#include "jf.h"
#include "player.h"
#include "util.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vita2d.h>

/* ------------------------------------------------------------------ theme */
#define C_BG RGBA8(13, 15, 21, 255)
#define C_PANEL RGBA8(26, 30, 40, 255)
#define C_CARD RGBA8(34, 39, 52, 255)
#define C_ACCENT RGBA8(0, 164, 220, 255)
#define C_TEXT RGBA8(240, 243, 250, 255)
#define C_DIM RGBA8(140, 150, 170, 255)
#define C_SHADE RGBA8(0, 0, 0, 170)

#define HEADER_H 52
#define MINI_H 44
#define COLS 6
#define CW 134
#define CH 201
#define GAP 14
#define ROWH (CH + 42)
#define X0 ((SCR_W - COLS * CW - (COLS - 1) * GAP) / 2)
#define LIST_H 58

static vita2d_pgf *font;

static void txt(int x, int y, uint32_t c, float s, const char *t) {
  vita2d_pgf_draw_text(font, x, y, c, s, t);
}
static int tw(float s, const char *t) { return vita2d_pgf_text_width(font, s, t); }

static void txt_fit(int x, int y, uint32_t c, float s, const char *t, int maxw) {
  char b[200];
  snprintf(b, sizeof b, "%s", t);
  int l = (int)strlen(b);
  if (tw(s, b) > maxw) {
    while (l > 3 && tw(s, b) + tw(s, "...") > maxw) b[--l] = 0;
    strcat(b, "...");
  }
  txt(x, y, c, s, b);
}

static void txt_center(int cx, int y, uint32_t c, float s, const char *t) {
  txt(cx - tw(s, t) / 2, y, c, s, t);
}

static void wrap(int x, int y, int w, int lh, int maxlines, uint32_t c, float s, const char *t) {
  char line[256] = "";
  char word[96];
  const char *p = t;
  int lines = 0;
  while (*p && lines < maxlines) {
    int k = 0;
    while (*p && *p != ' ' && *p != '\n' && k < 95) word[k++] = *p++;
    word[k] = 0;
    while (*p == ' ' || *p == '\n') p++;
    char trial[300];
    snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
    if (tw(s, trial) > w && line[0]) {
      txt(x, y + lines * lh, c, s, line);
      lines++;
      snprintf(line, sizeof line, "%s", word);
    } else {
      snprintf(line, sizeof line, "%s", trial);
    }
  }
  if (line[0] && lines < maxlines) txt(x, y + lines * lh, c, s, line);
}

static void fmt_time(char *o, size_t n, uint64_t ms) {
  unsigned s = (unsigned)(ms / 1000);
  if (s >= 3600) snprintf(o, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(o, n, "%u:%02u", s / 60, s % 60);
}

static void spinner(int cx, int cy) {
  static int t;
  t++;
  for (int i = 0; i < 8; i++) {
    int a = 255 - ((t / 3 + 8 - i) % 8) * 28;
    if (a < 40) a = 40;
    float ang = i * 0.785398f;
    int dx = (int)(18 * __builtin_sinf(ang)), dy = (int)(-18 * __builtin_cosf(ang));
    vita2d_draw_rectangle(cx + dx - 3, cy + dy - 3, 6, 6, RGBA8(0, 164, 220, a));
  }
}

/* ------------------------------------------------------------------ input */
typedef struct {
  uint32_t pressed, held;
  int touching, tap, tx, ty, dy;
} input_t;

static input_t in;
static uint32_t prev_buttons, hold_frames[32];

static int rep(uint32_t b) {
  if (in.pressed & b) return 1;
  if (!(in.held & b)) return 0;
  for (int i = 0; i < 32; i++)
    if (b == (1u << i)) return hold_frames[i] > 18 && hold_frames[i] % 5 == 0;
  return 0;
}

static void poll_input(void) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(0, &pad, 1);
  in.held = pad.buttons;
  in.pressed = pad.buttons & ~prev_buttons;
  prev_buttons = pad.buttons;
  for (int i = 0; i < 32; i++) hold_frames[i] = (pad.buttons & (1u << i)) ? hold_frames[i] + 1 : 0;

  static int was_down, sx, sy, lx, ly, moved, frames;
  SceTouchData td;
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
  int down = td.reportNum > 0;
  int x = down ? td.report[0].x / 2 : lx, y = down ? td.report[0].y / 2 : ly;
  in.tap = 0;
  in.dy = 0;
  if (down && !was_down) {
    sx = lx = x;
    sy = ly = y;
    moved = 0;
    frames = 0;
  } else if (down) {
    in.dy = y - ly;
    moved += abs(y - ly) + abs(x - lx);
    lx = x;
    ly = y;
    frames++;
  } else if (was_down && moved < 14 && frames < 25) {
    in.tap = 1;
    in.tx = sx;
    in.ty = sy;
  }
  in.touching = down && moved >= 14;
  was_down = down;
}

/* ------------------------------------------------------------------ state */
typedef enum { S_LOGIN, S_BROWSE, S_DETAIL, S_PLAYER } screen_t;
static screen_t screen = S_LOGIN;

static void spawn(void *(*fn)(void *), void *arg) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 * 1024);
  pthread_t t;
  if (pthread_create(&t, &a, fn, arg) == 0) pthread_detach(t);
}

/* ---- login ---- */
static volatile int login_state; /* 1 working, 2 show code, 3 done, 4 error, 5 no config */
static char login_code[16], login_msg[200];

static void *login_thread(void *a) {
  (void)a;
  if (jf_have_token()) {
    login_state = 3;
    return NULL;
  }
  if (jf_has_password_login()) {
    snprintf(login_msg, sizeof login_msg, "Signing in...");
    if (jf_login_password() == 0) login_state = 3;
    else {
      snprintf(login_msg, sizeof login_msg,
               "Sign-in failed. Check server, username and password in config.json.");
      login_state = 4;
    }
    return NULL;
  }
  char secret[96];
  if (jf_quick_start(login_code, sizeof login_code, secret, sizeof secret) != 0) {
    snprintf(login_msg, sizeof login_msg,
             "Could not reach the server, or Quick Connect is turned off in the Jellyfin dashboard.");
    login_state = 4;
    return NULL;
  }
  login_state = 2;
  for (int i = 0; i < 100; i++) {
    sleep(3);
    if (jf_quick_poll(secret)) {
      login_state = 3;
      return NULL;
    }
  }
  snprintf(login_msg, sizeof login_msg, "Quick Connect code expired.");
  login_state = 4;
  return NULL;
}

static void login_begin(void) {
  login_state = 1;
  snprintf(login_msg, sizeof login_msg, "Connecting...");
  spawn(login_thread, NULL);
}

/* ---- views ---- */
typedef struct view {
  jf_list l;
  char title[96], parent[JF_ID], types[64];
  volatile int state, dead; /* state: 1 loading, 2 ready, 3 error */
  int styled, list_style, sel;
  float scroll, vel;
} view;

static view *stack[8];
static int depth;
static jf_list tabs;
static volatile int tabs_state;
static int tab_sel;

static void *view_thread(void *a) {
  view *v = a;
  int rc = v->parent[0] ? jf_children(v->parent, v->types, 0, 300, &v->l) : jf_resume(&v->l);
  v->state = rc == 0 ? 2 : 3;
  if (v->dead) {
    jf_list_free(&v->l);
    free(v);
  }
  return NULL;
}

static view *view_new(const char *title, const char *parent, const char *types) {
  view *v = calloc(1, sizeof *v);
  snprintf(v->title, sizeof v->title, "%s", title);
  snprintf(v->parent, sizeof v->parent, "%s", parent);
  snprintf(v->types, sizeof v->types, "%s", types);
  v->state = 1;
  spawn(view_thread, v);
  return v;
}

static void view_drop(view *v) {
  if (v->state == 1) v->dead = 1;
  else {
    jf_list_free(&v->l);
    free(v);
  }
}

static void *tabs_thread(void *a) {
  (void)a;
  tabs_state = jf_views(&tabs) == 0 ? 2 : 3;
  return NULL;
}

static void load_tab(int i) {
  while (depth > 0) view_drop(stack[--depth]);
  tab_sel = i;
  if (i == 0) stack[0] = view_new("Continue Watching", "", "");
  else stack[0] = view_new(tabs.v[i - 1].name, tabs.v[i - 1].id, "Movie,Series,MusicAlbum");
  depth = 1;
}

static void browse_begin(void) {
  tabs_state = 1;
  spawn(tabs_thread, NULL);
  screen = S_BROWSE;
}

/* ---- detail ---- */
typedef struct {
  jf_item it;
  jf_detail d;
  volatile int state, dead;
} det_t;
static det_t *det;
static int sub_sel = -1;

static void *det_thread(void *a) {
  det_t *d = a;
  d->state = jf_detail_get(d->it.id, &d->d) == 0 ? 2 : 3;
  if (d->dead) free(d);
  return NULL;
}

static void detail_open(const jf_item *it) {
  det = calloc(1, sizeof *det);
  det->it = *it;
  det->state = 1;
  sub_sel = -1;
  spawn(det_thread, det);
  screen = S_DETAIL;
}

static void detail_close(void) {
  if (det->state == 1) det->dead = 1;
  else free(det);
  det = NULL;
  screen = S_BROWSE;
}

/* ---- playback / music ---- */
static jf_item *mq;
static int mq_n, mq_i, music_on;
static jf_item vid_it;
static jf_detail vid_d;
static int vid_resume_ms, vid_seek_pending;
static float overlay_t;
static uint32_t last_report;

typedef struct {
  char id[JF_ID], sid[JF_ID];
  int64_t pos;
  int state, paused;
} rep_t;

static void *report_thread(void *a) {
  rep_t *r = a;
  jf_report(r->id, r->sid, r->pos, r->state, r->paused);
  free(r);
  return NULL;
}

static void report(int state) {
  rep_t *r = calloc(1, sizeof *r);
  snprintf(r->id, sizeof r->id, "%s", vid_it.id);
  snprintf(r->sid, sizeof r->sid, "%s", vid_d.source_id);
  r->pos = (int64_t)pl_time() * TICKS_PER_MS;
  r->state = state;
  r->paused = pl_paused();
  spawn(report_thread, r);
}

static void music_play_index(int i) {
  char url[512];
  mq_i = i;
  jf_url_audio(mq[i].id, url, sizeof url);
  music_on = pl_open(url, 0) == 0;
}

static void music_start(const view *v, int idx) {
  free(mq);
  mq_n = 0;
  mq = calloc(v->l.n, sizeof(jf_item));
  int start = 0;
  for (int i = 0; i < v->l.n; i++)
    if (!strcmp(v->l.v[i].type, "Audio")) {
      if (i == idx) start = mq_n;
      mq[mq_n++] = v->l.v[i];
    }
  if (mq_n) music_play_index(start);
}

static void music_stop(void) {
  pl_close();
  music_on = 0;
}

static void video_start(void) {
  char url[1536];
  music_stop();
  vid_it = det->it;
  vid_d = det->d;
  jf_url_video(vid_it.id, &vid_d, url, sizeof url);
  if (pl_open(url, 1) != 0) return;
  pl_subs_clear();
  if (sub_sel >= 0) {
    char su[768];
    jf_url_sub(vid_it.id, &vid_d, vid_d.subs[sub_sel].index, su, sizeof su);
    pl_subs_load(su);
  }
  vid_resume_ms = (int)(vid_it.resume / TICKS_PER_MS);
  if (vid_it.runtime && vid_it.resume > vid_it.runtime / 50 &&
      vid_it.resume < vid_it.runtime - vid_it.runtime / 20)
    vid_seek_pending = 1;
  else vid_seek_pending = 0;
  overlay_t = 4.0f;
  last_report = sceKernelGetProcessTimeLow();
  report(0);
  screen = S_PLAYER;
}

static void video_stop(void) {
  report(2);
  pl_close();
  pl_subs_clear();
  screen = S_DETAIL;
}

/* ------------------------------------------------------------------ browse */
static int is_container(const char *t) {
  return !strcmp(t, "Series") || !strcmp(t, "Season") || !strcmp(t, "MusicAlbum") ||
         !strcmp(t, "BoxSet") || !strcmp(t, "Folder") || !strcmp(t, "CollectionFolder") ||
         !strcmp(t, "Playlist") || !strcmp(t, "MusicArtist");
}

static void activate(view *v, int i) {
  jf_item *it = &v->l.v[i];
  if (is_container(it->type)) {
    if (depth < 8) stack[depth++] = view_new(it->name, it->id, "");
  } else if (!strcmp(it->type, "Audio")) {
    music_start(v, i);
  } else {
    detail_open(it);
  }
}

static void item_label(const jf_item *it, char *o, size_t n) {
  if (!strcmp(it->type, "Episode"))
    snprintf(o, n, "S%dE%d  %s", it->parent_index, it->index, it->name);
  else if (!strcmp(it->type, "Audio") && it->index) snprintf(o, n, "%d.  %s", it->index, it->name);
  else snprintf(o, n, "%s", it->name);
}

static int content_bottom(void) { return SCR_H - (music_on ? MINI_H : 0); }

static void draw_poster(const jf_item *it, int x, int y, int sel) {
  vita2d_draw_rectangle(x, y, CW, CH, C_CARD);
  vita2d_texture *t = img_get(it->id, 200);
  if (t) {
    float tw_ = vita2d_texture_get_width(t), th_ = vita2d_texture_get_height(t);
    float s = CW / tw_ < CH / th_ ? CW / tw_ : CH / th_;
    vita2d_draw_texture_scale(t, x + (CW - tw_ * s) / 2, y + (CH - th_ * s) / 2, s, s);
  }
  if (it->resume && it->runtime) {
    int w = (int)((float)it->resume / it->runtime * CW);
    vita2d_draw_rectangle(x, y + CH - 5, CW, 5, RGBA8(0, 0, 0, 180));
    vita2d_draw_rectangle(x, y + CH - 5, w, 5, C_ACCENT);
  }
  if (it->played) {
    vita2d_draw_rectangle(x + CW - 24, y + 6, 18, 18, C_ACCENT);
    txt(x + CW - 21, y + 20, C_TEXT, 0.9f, "v");
  }
  if (sel) {
    vita2d_draw_rectangle(x - 3, y - 3, CW + 6, 3, C_ACCENT);
    vita2d_draw_rectangle(x - 3, y + CH, CW + 6, 3, C_ACCENT);
    vita2d_draw_rectangle(x - 3, y - 3, 3, CH + 6, C_ACCENT);
    vita2d_draw_rectangle(x + CW, y - 3, 3, CH + 6, C_ACCENT);
  }
  char lab[200];
  item_label(it, lab, sizeof lab);
  txt_fit(x, y + CH + 20, sel ? C_TEXT : C_DIM, 0.8f, lab, CW);
  if (it->year) {
    char y_[12];
    snprintf(y_, sizeof y_, "%d", it->year);
    txt(x, y + CH + 37, C_DIM, 0.65f, y_);
  } else if (it->series[0]) {
    txt_fit(x, y + CH + 37, C_DIM, 0.65f, it->series, CW);
  }
}

static void draw_list_row(const jf_item *it, int y, int sel) {
  vita2d_draw_rectangle(24, y, SCR_W - 48, LIST_H - 6, sel ? RGBA8(0, 90, 125, 255) : C_CARD);
  char lab[200];
  item_label(it, lab, sizeof lab);
  txt_fit(44, y + 28, C_TEXT, 0.95f, lab, SCR_W - 260);
  if (it->runtime) {
    char m[24];
    snprintf(m, sizeof m, "%d min", (int)(it->runtime / TICKS_PER_MS / 60000));
    txt(SCR_W - 44 - tw(0.8f, m), y + 28, C_DIM, 0.8f, m);
  }
  if (it->resume && it->runtime) {
    int w = (int)((float)it->resume / it->runtime * (SCR_W - 48));
    vita2d_draw_rectangle(24, y + LIST_H - 10, w, 4, C_ACCENT);
  }
  if (it->played) txt(SCR_W - 44 - 100, y + 28, C_ACCENT, 0.9f, "v");
}

static int music_mini_tap(void) {
  return in.tap && music_on && in.ty >= SCR_H - MINI_H;
}

static void draw_mini(void) {
  if (!music_on) return;
  int y = SCR_H - MINI_H;
  vita2d_draw_rectangle(0, y, SCR_W, MINI_H, C_PANEL);
  vita2d_draw_rectangle(0, y, SCR_W, 2, C_ACCENT);
  txt_fit(20, y + 29, C_TEXT, 0.9f, mq[mq_i].name, 560);
  char a[16], b[16], t[40];
  fmt_time(a, sizeof a, pl_time());
  fmt_time(b, sizeof b, mq[mq_i].runtime / TICKS_PER_MS);
  snprintf(t, sizeof t, "%s / %s", a, b);
  txt(SCR_W - 20 - tw(0.8f, t), y + 28, C_DIM, 0.8f, t);
  txt(640, y + 28, C_DIM, 0.7f, pl_paused() ? "[Tri] resume" : "[Tri] pause");
}

static void music_tick(void) {
  if (!music_on) return;
  if (in.pressed & SCE_CTRL_TRIANGLE) pl_toggle_pause();
  if (music_mini_tap()) pl_toggle_pause();
  if ((in.pressed & SCE_CTRL_SELECT) || pl_ended()) {
    if (mq_i + 1 < mq_n) music_play_index(mq_i + 1);
    else music_stop();
  }
}

static void browse_frame(void) {
  vita2d_clear_screen();
  view *v = depth ? stack[depth - 1] : NULL;

  if (tabs_state == 3 || (v && v->state == 3)) {
    txt_center(SCR_W / 2, 250, C_TEXT, 1.2f, "Could not load from your server.");
    txt_center(SCR_W / 2, 290, C_DIM, 0.9f, "Press [X] to sign in again, or [O] to go back.");
    if (in.pressed & SCE_CTRL_CROSS) {
      jf_logout();
      while (depth > 0) view_drop(stack[--depth]);
      screen = S_LOGIN;
      login_begin();
    }
    if ((in.pressed & SCE_CTRL_CIRCLE) && depth > 1) view_drop(stack[--depth]);
    return;
  }

  /* initial tab load */
  if (tabs_state == 2 && depth == 0) load_tab(0);
  if (!v) {
    spinner(SCR_W / 2, SCR_H / 2);
    return;
  }

  /* tab switching (root only) */
  if (depth == 1 && tabs_state == 2) {
    int nt = tabs.n + 1;
    if (in.pressed & SCE_CTRL_LTRIGGER) load_tab((tab_sel + nt - 1) % nt);
    else if (in.pressed & SCE_CTRL_RTRIGGER) load_tab((tab_sel + 1) % nt);
    v = stack[depth - 1];
  }

  if (v->state == 2 && !v->styled) {
    v->styled = 1;
    if (v->l.n) {
      const char *t = v->l.v[0].type;
      v->list_style = v->parent[0] && (!strcmp(t, "Episode") || !strcmp(t, "Audio") ||
                                       !strcmp(t, "Season"));
    }
  }

  int bottom = content_bottom();
  int vh = bottom - HEADER_H;
  int per_row = v->list_style ? 1 : COLS;
  int rows = (v->l.n + per_row - 1) / per_row;
  int rowh = v->list_style ? LIST_H : ROWH;
  float total = rows * rowh + 24;
  float maxs = total > vh ? total - vh : 0;

  if (v->state == 2 && v->l.n) {
    int n = v->l.n;
    if (v->list_style) {
      if (rep(SCE_CTRL_DOWN) && v->sel + 1 < n) v->sel++;
      if (rep(SCE_CTRL_UP) && v->sel > 0) v->sel--;
    } else {
      if (rep(SCE_CTRL_RIGHT) && v->sel + 1 < n) v->sel++;
      if (rep(SCE_CTRL_LEFT) && v->sel > 0) v->sel--;
      if (rep(SCE_CTRL_DOWN) && v->sel + COLS < n) v->sel += COLS;
      if (rep(SCE_CTRL_UP) && v->sel - COLS >= 0) v->sel -= COLS;
    }
    if (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
      float top = 12 + (v->sel / per_row) * rowh;
      if (top < v->scroll) v->scroll = top - 8;
      if (top + rowh > v->scroll + vh) v->scroll = top + rowh - vh + 8;
      v->vel = 0;
    }
  }

  /* touch scroll + inertia */
  if (in.touching) {
    v->scroll -= in.dy;
    v->vel = -in.dy;
  } else {
    v->scroll += v->vel;
    v->vel *= 0.93f;
    if (v->vel > -0.3f && v->vel < 0.3f) v->vel = 0;
  }
  if (v->scroll < 0) v->scroll = 0, v->vel = 0;
  if (v->scroll > maxs) v->scroll = maxs, v->vel = 0;

  /* taps */
  if (in.tap && in.ty < HEADER_H && depth == 1 && tabs_state == 2) {
    int x = 130;
    int nt = tabs.n + 1;
    for (int i = 0; i < nt; i++) {
      const char *nm = i == 0 ? "Continue" : tabs.v[i - 1].name;
      int w = tw(0.9f, nm) + 28;
      if (in.tx >= x && in.tx < x + w) {
        if (i != tab_sel) load_tab(i);
        break;
      }
      x += w;
    }
    v = stack[depth - 1];
  } else if (in.tap && in.ty >= HEADER_H && in.ty < bottom && v->state == 2) {
    int idx = -1;
    float yy = in.ty - HEADER_H + v->scroll - 12;
    if (v->list_style) idx = (int)(yy / LIST_H);
    else {
      int col = (in.tx - X0) / (CW + GAP);
      int row = (int)(yy / ROWH);
      if (in.tx >= X0 && col >= 0 && col < COLS && yy >= 0) idx = row * COLS + col;
    }
    if (idx >= 0 && idx < v->l.n) {
      v->sel = idx;
      activate(v, idx);
    }
  } else if ((in.pressed & SCE_CTRL_CROSS) && v->state == 2 && v->l.n) {
    activate(v, v->sel);
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) && depth > 1) {
    view_drop(stack[--depth]);
    return;
  }
  if (in.tap && in.ty < HEADER_H && in.tx < 110 && depth > 1) {
    view_drop(stack[--depth]);
    return;
  }
  v = stack[depth - 1];

  /* draw content */
  vita2d_enable_clipping();
  vita2d_set_clip_rectangle(0, HEADER_H, SCR_W, bottom);
  if (v->state == 1) spinner(SCR_W / 2, SCR_H / 2);
  else if (!v->l.n) txt_center(SCR_W / 2, 280, C_DIM, 1.0f, "Nothing here yet.");
  else if (v->list_style) {
    int first = (int)((v->scroll - 12) / LIST_H);
    if (first < 0) first = 0;
    for (int i = first; i < v->l.n; i++) {
      int y = HEADER_H + 12 + i * LIST_H - (int)v->scroll;
      if (y > bottom) break;
      draw_list_row(&v->l.v[i], y, i == v->sel);
    }
  } else {
    int first = (int)((v->scroll - 12) / ROWH);
    if (first < 0) first = 0;
    for (int r = first; r * COLS < v->l.n; r++) {
      int y = HEADER_H + 12 + r * ROWH - (int)v->scroll;
      if (y > bottom) break;
      for (int c = 0; c < COLS && r * COLS + c < v->l.n; c++) {
        int i = r * COLS + c;
        draw_poster(&v->l.v[i], X0 + c * (CW + GAP), y, i == v->sel);
      }
    }
  }
  vita2d_disable_clipping();

  /* header */
  vita2d_draw_rectangle(0, 0, SCR_W, HEADER_H, C_PANEL);
  vita2d_draw_rectangle(0, HEADER_H - 2, SCR_W, 2, RGBA8(0, 0, 0, 80));
  if (depth > 1) {
    txt(20, 34, C_ACCENT, 1.0f, "< Back");
    txt_fit(130, 34, C_TEXT, 1.0f, v->title, 700);
  } else {
    txt(20, 34, C_ACCENT, 1.1f, "VitaFin");
    int x = 130;
    for (int i = 0; tabs_state == 2 && i < tabs.n + 1; i++) {
      const char *nm = i == 0 ? "Continue" : tabs.v[i - 1].name;
      int w = tw(0.9f, nm) + 28;
      if (i == tab_sel) vita2d_draw_rectangle(x, HEADER_H - 5, w, 3, C_ACCENT);
      txt(x + 14, 33, i == tab_sel ? C_TEXT : C_DIM, 0.9f, nm);
      x += w;
    }
  }
  draw_mini();
}

/* ------------------------------------------------------------------ detail */
static void detail_frame(void) {
  vita2d_clear_screen();
  if (!det) return;
  vita2d_draw_rectangle(0, 0, SCR_W, SCR_H, C_BG);

  vita2d_texture *t = img_get(det->it.id, 300);
  int px = 40, py = 50, pw = 250, ph = 375;
  vita2d_draw_rectangle(px, py, pw, ph, C_CARD);
  if (t) {
    float tw_ = vita2d_texture_get_width(t), th_ = vita2d_texture_get_height(t);
    float s = pw / tw_ < ph / th_ ? pw / tw_ : ph / th_;
    vita2d_draw_texture_scale(t, px + (pw - tw_ * s) / 2, py + (ph - th_ * s) / 2, s, s);
  }

  int x = 330;
  char lab[200];
  if (det->it.series[0]) txt(x, 62, C_ACCENT, 0.9f, det->it.series);
  item_label(&det->it, lab, sizeof lab);
  wrap(x, 100, 590, 36, 2, C_TEXT, 1.4f, lab);

  char meta[96] = "";
  if (det->it.year) snprintf(meta, sizeof meta, "%d", det->it.year);
  if (det->it.runtime) {
    char r[32];
    snprintf(r, sizeof r, "%s%d min", meta[0] ? "   " : "", (int)(det->it.runtime / TICKS_PER_MS / 60000));
    strcat(meta, r);
  }
  txt(x, 180, C_DIM, 0.9f, meta);

  if (det->state == 1) spinner(x + 200, 300);
  else if (det->state == 3) txt(x, 260, C_DIM, 0.9f, "Could not load details.");
  else {
    wrap(x, 215, 590, 24, 7, C_TEXT, 0.85f, det->d.overview);
    char info[160];
    snprintf(info, sizeof info, "%s %s %dx%d   %s", det->d.container, det->d.vcodec, det->d.width,
             det->d.height, det->d.acodec);
    txt(x, 420, C_DIM, 0.7f, info);
  }

  /* buttons */
  int by = 450;
  const char *pl = det->it.resume ? "[X] Resume" : "[X] Play";
  vita2d_draw_rectangle(x, by, 170, 44, C_ACCENT);
  txt_center(x + 85, by + 29, C_TEXT, 1.0f, pl);
  char sb[96];
  if (det->state == 2 && det->d.nsubs) {
    if (sub_sel < 0) snprintf(sb, sizeof sb, "[Sq] Subtitles: Off");
    else snprintf(sb, sizeof sb, "[Sq] Subs: %s", det->d.subs[sub_sel].lang[0] ? det->d.subs[sub_sel].lang : "track");
    vita2d_draw_rectangle(x + 190, by, 260, 44, C_CARD);
    txt_center(x + 320, by + 29, C_TEXT, 0.9f, sb);
  }
  txt(SCR_W - 120, by + 29, C_DIM, 0.9f, "[O] Back");

  int play = (in.pressed & SCE_CTRL_CROSS) ||
             (in.tap && in.tx >= x && in.tx < x + 170 && in.ty >= by && in.ty < by + 44);
  int subs = (in.pressed & SCE_CTRL_SQUARE) ||
             (in.tap && in.tx >= x + 190 && in.tx < x + 450 && in.ty >= by && in.ty < by + 44);
  if (subs && det->state == 2 && det->d.nsubs) {
    sub_sel++;
    if (sub_sel >= det->d.nsubs) sub_sel = -1;
  }
  if (play && det->state == 2) {
    video_start();
    return;
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && in.tx > SCR_W - 140 && in.ty > by)) {
    detail_close();
  }
}

/* ------------------------------------------------------------------ player */
static void player_frame(void) {
  pl_update();
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
  vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
  vita2d_clear_screen();
  pl_draw_video(0, 0, SCR_W, SCR_H);

  uint64_t now = pl_time();
  uint64_t dur = vid_it.runtime / TICKS_PER_MS;

  if (vid_seek_pending && pl_has_frame()) {
    pl_seek_to(vid_resume_ms);
    vid_seek_pending = 0;
  }

  if (!pl_has_frame()) spinner(SCR_W / 2, SCR_H / 2);

  /* subtitles */
  const char *sub = sub_sel >= 0 ? pl_sub_at(now) : NULL;
  if (sub) {
    char buf[200];
    snprintf(buf, sizeof buf, "%s", sub);
    int lines = 1;
    for (char *c = buf; *c; c++) lines += *c == '\n';
    char *line = buf;
    int y = SCR_H - 40 - (lines - 1) * 30 - (overlay_t > 0 ? 60 : 0);
    for (int i = 0; i < lines; i++) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = 0;
      int w = tw(1.1f, line);
      txt(SCR_W / 2 - w / 2 + 2, y + 2, RGBA8(0, 0, 0, 255), 1.1f, line);
      txt(SCR_W / 2 - w / 2, y, RGBA8(255, 255, 255, 255), 1.1f, line);
      y += 30;
      line = nl ? nl + 1 : line + strlen(line);
    }
  }

  /* input */
  if (in.pressed & SCE_CTRL_CROSS) {
    pl_toggle_pause();
    overlay_t = 4;
  }
  if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_LEFT)) {
    pl_seek_to(now > 10000 ? now - 10000 : 0);
    overlay_t = 3;
  }
  if (in.pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_RIGHT)) {
    pl_seek_to(now + 10000);
    overlay_t = 3;
  }
  if (in.pressed & SCE_CTRL_SQUARE) {
    sub_sel++;
    if (sub_sel >= vid_d.nsubs) sub_sel = -1;
    pl_subs_clear();
    if (sub_sel >= 0) {
      char su[768];
      jf_url_sub(vid_it.id, &vid_d, vid_d.subs[sub_sel].index, su, sizeof su);
      pl_subs_load(su);
    }
    overlay_t = 3;
  }
  if (in.tap) {
    if (overlay_t <= 0) overlay_t = 4;
    else if (in.ty > SCR_H - 70 && dur) {
      float f = (float)(in.tx - 60) / (SCR_W - 120);
      if (f < 0) f = 0;
      if (f > 1) f = 1;
      pl_seek_to((uint64_t)(f * dur));
      overlay_t = 4;
    } else {
      pl_toggle_pause();
      overlay_t = 4;
    }
  }

  /* overlay */
  if (overlay_t > 0 || pl_paused()) {
    vita2d_draw_rectangle(0, 0, SCR_W, 56, C_SHADE);
    char lab[200];
    item_label(&vid_it, lab, sizeof lab);
    txt_fit(24, 36, C_TEXT, 1.0f, lab, SCR_W - 200);
    vita2d_draw_rectangle(0, SCR_H - 80, SCR_W, 80, C_SHADE);
    char a[16], b[16];
    fmt_time(a, sizeof a, now);
    fmt_time(b, sizeof b, dur);
    txt(24, SCR_H - 24, C_TEXT, 0.85f, a);
    txt(SCR_W - 24 - tw(0.85f, b), SCR_H - 24, C_TEXT, 0.85f, b);
    vita2d_draw_rectangle(60, SCR_H - 34, SCR_W - 120, 6, RGBA8(255, 255, 255, 70));
    if (dur) {
      float f = (float)now / dur;
      if (f > 1) f = 1;
      vita2d_draw_rectangle(60, SCR_H - 34, (int)((SCR_W - 120) * f), 6, C_ACCENT);
      vita2d_draw_fill_circle(60 + (SCR_W - 120) * f, SCR_H - 31, 8, C_ACCENT);
    }
    txt_center(SCR_W / 2, SCR_H - 52, C_DIM, 0.7f,
               "[X] pause   [L/R] -/+10s   [Sq] subtitles   [O] stop");
    if (pl_paused()) txt_center(SCR_W / 2, SCR_H / 2, C_TEXT, 2.0f, "||");
  }
  if (overlay_t > 0) overlay_t -= 1.0f / 60;

  /* progress report every 10 s */
  uint32_t t = sceKernelGetProcessTimeLow();
  if (t - last_report > 10000000u) {
    last_report = t;
    report(1);
  }

  if ((in.pressed & SCE_CTRL_CIRCLE) || pl_ended()) {
    video_stop();
    vita2d_set_clear_color(C_BG);
  }
}

/* ------------------------------------------------------------------ login screen */
static void login_frame(void) {
  vita2d_clear_screen();
  txt_center(SCR_W / 2, 140, C_ACCENT, 2.2f, "VitaFin");
  if (!jf_server_set()) {
    txt_center(SCR_W / 2, 230, C_TEXT, 1.0f, "Set your server address first.");
    txt_center(SCR_W / 2, 270, C_DIM, 0.85f, "Edit  ux0:data/VitaFin/config.json  with VitaShell:");
    txt_center(SCR_W / 2, 310, C_DIM, 0.85f, "{ \"server\": \"http://192.168.1.10:8096\" }");
    txt_center(SCR_W / 2, 360, C_DIM, 0.8f, "Then restart the app.");
    return;
  }
  if (login_state == 2) {
    txt_center(SCR_W / 2, 230, C_TEXT, 1.0f, "On another device, open Jellyfin, then:");
    txt_center(SCR_W / 2, 262, C_DIM, 0.85f, "Profile > Quick Connect, and enter this code");
    txt_center(SCR_W / 2, 360, C_TEXT, 3.0f, login_code);
    spinner(SCR_W / 2, 440);
  } else if (login_state == 4) {
    wrap(180, 260, 600, 30, 3, C_TEXT, 0.95f, login_msg);
    txt_center(SCR_W / 2, 400, C_DIM, 0.9f, "Press [X] to try again");
    if (in.pressed & SCE_CTRL_CROSS) login_begin();
  } else {
    txt_center(SCR_W / 2, 280, C_DIM, 0.95f, login_msg);
    spinner(SCR_W / 2, 360);
  }
  if (login_state == 3) browse_begin();
}

/* ------------------------------------------------------------------ entry */
void ui_init(void) {
  font = vita2d_load_default_pgf();
  vita2d_set_clear_color(C_BG);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  img_init();
  if (jf_server_set()) login_begin();
}

void ui_frame(void) {
  poll_input();
  if (music_on) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

  if (screen == S_BROWSE || screen == S_DETAIL) music_tick();

  vita2d_start_drawing();
  switch (screen) {
    case S_LOGIN: login_frame(); break;
    case S_BROWSE: browse_frame(); break;
    case S_DETAIL: detail_frame(); break;
    case S_PLAYER: player_frame(); break;
  }
  if (screen == S_DETAIL) draw_mini();
  vita2d_end_drawing();
  vita2d_swap_buffers();
}
