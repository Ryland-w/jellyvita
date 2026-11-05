#pragma once
#include <stdint.h>

int pl_open(const char *url, int is_video);   /* 0 ok */
void pl_close(void);
int pl_active(void);                           /* a source is loaded */
void pl_update(void);                          /* call once per frame */
void pl_draw_video(int x, int y, int w, int h);
void pl_toggle_pause(void);
int pl_paused(void);
void pl_seek_to(uint64_t ms);
uint64_t pl_time(void);                        /* ms */
int pl_ended(void);
int pl_has_frame(void);

void pl_subs_load(const char *url);
void pl_subs_clear(void);
const char *pl_sub_at(uint64_t ms);            /* NULL if none */
