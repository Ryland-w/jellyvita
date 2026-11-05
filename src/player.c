#include "player.h"
#include "jf.h"

#include <malloc.h>
#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

/* Playback uses the Vita's own SceAvPlayer (hardware H.264 + AAC/MP3). Data is pulled from
   Jellyfin with HTTP range requests through the file-replacement callbacks below. */

static SceAvPlayerHandle h;
static volatile int run;
static int paused, video_mode;
static uint64_t opened_at;
static SceUID audio_tid = -1;

static vita2d_texture *vt;
static int vt_w, vt_h, got_frame;

/* ---- memory callbacks ---- */
static void *mem_alloc(void *p, uint32_t align, uint32_t size) {
  (void)p;
  return memalign(align, size);
}
static void mem_free(void *p, void *ptr) {
  (void)p;
  free(ptr);
}

static struct {
  void *base;
  SceUID uid;
} blocks[16];

static void *tex_alloc(void *p, uint32_t align, uint32_t size) {
  (void)p;
  (void)align;
  size = (size + 0x3FFFF) & ~0x3FFFFu;
  SceUID uid = sceKernelAllocMemBlock("avp", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
  if (uid < 0) return NULL;
  void *base = NULL;
  sceKernelGetMemBlockBase(uid, &base);
  sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
  for (int i = 0; i < 16; i++)
    if (!blocks[i].base) {
      blocks[i].base = base;
      blocks[i].uid = uid;
      break;
    }
  return base;
}

static void tex_free(void *p, void *ptr) {
  (void)p;
  for (int i = 0; i < 16; i++)
    if (blocks[i].base == ptr) {
      sceGxmUnmapMemory(ptr);
      sceKernelFreeMemBlock(blocks[i].uid);
      blocks[i].base = NULL;
      return;
    }
}

/* ---- HTTP-backed file ---- */
static char cur_url[1536];
static int64_t cur_size;

static int32_t f_open(void *p, const char *name) {
  (void)p;
  snprintf(cur_url, sizeof cur_url, "%s", name);
  cur_size = jf_http_size(cur_url);
  return cur_size > 0 ? 0 : -1;
}
static int32_t f_close(void *p) {
  (void)p;
  return 0;
}
static int32_t f_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
  (void)p;
  if ((int64_t)pos >= cur_size) return 0;
  if (pos + len > (uint64_t)cur_size) len = (uint32_t)(cur_size - pos);
  int n = jf_http_range(cur_url, pos, len, buf);
  return n < 0 ? 0 : n;
}
static uint64_t f_size(void *p) {
  (void)p;
  return (uint64_t)cur_size;
}

/* ---- audio thread ---- */
static int audio_main(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000,
                                 SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0) return 0;
  int vol[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
  sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
  int grain = 1024;
  static int16_t mono2st[8192 * 2];
  while (run) {
    SceAvPlayerFrameInfo f;
    if (h && !paused && sceAvPlayerIsActive(h) && sceAvPlayerGetAudioData(h, &f)) {
      int ch = f.details.audio.channelCount ? f.details.audio.channelCount : 2;
      int frames = f.details.audio.size / (ch * 2);
      void *data = f.pData;
      if (ch == 1 && frames <= 8192) {
        int16_t *s = (int16_t *)f.pData;
        for (int i = 0; i < frames; i++) mono2st[2 * i] = mono2st[2 * i + 1] = s[i];
        data = mono2st;
      }
      if (frames != grain && frames >= 64) {
        sceAudioOutSetConfig(port, frames, f.details.audio.sampleRate, SCE_AUDIO_OUT_MODE_STEREO);
        grain = frames;
      }
      sceAudioOutOutput(port, data);
    } else {
      sceKernelDelayThread(4000);
    }
  }
  sceAudioOutReleasePort(port);
  return 0;
}

/* ---- public API ---- */
int pl_open(const char *url, int is_video) {
  pl_close();
  SceAvPlayerInitData init;
  memset(&init, 0, sizeof init);
  init.memoryReplacement.allocate = mem_alloc;
  init.memoryReplacement.deallocate = mem_free;
  init.memoryReplacement.allocateTexture = tex_alloc;
  init.memoryReplacement.deallocateTexture = tex_free;
  init.fileReplacement.open = f_open;
  init.fileReplacement.close = f_close;
  init.fileReplacement.readOffset = f_read;
  init.fileReplacement.size = f_size;
  init.basePriority = 0xA0;
  init.numOutputVideoFrameBuffers = 3;
  init.autoStart = 1;
  h = sceAvPlayerInit(&init);
  if (!h) return -1;
  if (sceAvPlayerAddSource(h, url) < 0) {
    sceAvPlayerClose(h);
    h = 0;
    return -1;
  }
  video_mode = is_video;
  paused = 0;
  got_frame = 0;
  run = 1;
  opened_at = sceKernelGetSystemTimeWide();
  audio_tid = sceKernelCreateThread("vitafin_audio", audio_main, 0x10000100, 0x10000, 0, 0, NULL);
  if (audio_tid >= 0) sceKernelStartThread(audio_tid, 0, NULL);
  return 0;
}

void pl_close(void) {
  run = 0;
  if (audio_tid >= 0) {
    sceKernelWaitThreadEnd(audio_tid, NULL, NULL);
    sceKernelDeleteThread(audio_tid);
    audio_tid = -1;
  }
  if (h) {
    sceAvPlayerStop(h);
    sceAvPlayerClose(h);
    h = 0;
  }
  if (vt) {
    vita2d_wait_rendering_done();
    vita2d_free_texture(vt);
    vt = NULL;
  }
  got_frame = 0;
}

int pl_active(void) { return h != 0; }
int pl_has_frame(void) { return got_frame; }
int pl_paused(void) { return paused; }

void pl_update(void) {
  if (!h || !video_mode) return;
  SceAvPlayerFrameInfo f;
  if (!paused && sceAvPlayerIsActive(h) && sceAvPlayerGetVideoData(h, &f)) {
    int w = f.details.video.width, hh = f.details.video.height;
    if (!vt || w != vt_w || hh != vt_h) {
      if (vt) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(vt);
      }
      vt = vita2d_create_empty_texture_format(w, hh, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1);
      vt_w = w;
      vt_h = hh;
    }
    if (vt) {
      memcpy(vita2d_texture_get_datap(vt), f.pData, (size_t)w * hh * 3 / 2);
      got_frame = 1;
    }
  }
}

void pl_draw_video(int x, int y, int w, int hh) {
  if (!vt || !got_frame) return;
  float sx = (float)w / vt_w, sy = (float)hh / vt_h;
  float s = sx < sy ? sx : sy;
  float dw = vt_w * s, dh = vt_h * s;
  vita2d_draw_texture_scale(vt, x + (w - dw) / 2, y + (hh - dh) / 2, s, s);
}

void pl_toggle_pause(void) {
  if (!h) return;
  paused = !paused;
  if (paused) sceAvPlayerPause(h);
  else sceAvPlayerResume(h);
}

void pl_seek_to(uint64_t ms) {
  if (h) sceAvPlayerJumpToTime(h, ms);
}

uint64_t pl_time(void) { return h ? sceAvPlayerCurrentTime(h) : 0; }

int pl_ended(void) {
  if (!h) return 0;
  if (sceKernelGetSystemTimeWide() - opened_at < 4000000ULL) return 0;
  return !sceAvPlayerIsActive(h);
}

/* ---- SRT subtitles ---- */
typedef struct {
  uint32_t start, end;
  char text[200];
} cue;
static cue *cues;
static int ncues;

void pl_subs_clear(void) {
  free(cues);
  cues = NULL;
  ncues = 0;
}

static int parse_ts(const char *s, uint32_t *ms) {
  int hh, mm, ss, mmm;
  if (sscanf(s, "%d:%d:%d,%d", &hh, &mm, &ss, &mmm) != 4 &&
      sscanf(s, "%d:%d:%d.%d", &hh, &mm, &ss, &mmm) != 4)
    return 0;
  *ms = ((hh * 60 + mm) * 60 + ss) * 1000 + mmm;
  return 1;
}

void pl_subs_load(const char *url) {
  pl_subs_clear();
  uint8_t *buf;
  size_t len;
  if (jf_http_get(url, &buf, &len) != 0) return;
  buf = realloc(buf, len + 1);
  buf[len] = 0;
  cues = calloc(4000, sizeof(cue));
  char *p = (char *)buf;
  while (p && *p && ncues < 4000) {
    char *eol = strchr(p, '\n');
    if (eol) *eol = 0;
    char *arrow = strstr(p, "-->");
    if (arrow) {
      cue *c = &cues[ncues];
      if (parse_ts(p, &c->start) && parse_ts(arrow + 3 + (arrow[3] == ' '), &c->end)) {
        size_t o = 0;
        p = eol ? eol + 1 : NULL;
        while (p && *p) {
          eol = strchr(p, '\n');
          if (eol) *eol = 0;
          size_t l = strlen(p);
          while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
          if (!l) break;
          int in_tag = 0;
          for (size_t i = 0; i < l && o + 2 < sizeof c->text; i++) {
            if (p[i] == '<' || p[i] == '{') in_tag = 1;
            else if (p[i] == '>' || p[i] == '}') in_tag = 0;
            else if (!in_tag) c->text[o++] = p[i];
          }
          if (o + 2 < sizeof c->text) c->text[o++] = '\n';
          p = eol ? eol + 1 : NULL;
        }
        if (o && c->text[o - 1] == '\n') o--;
        c->text[o] = 0;
        ncues++;
        continue;
      }
    }
    p = eol ? eol + 1 : NULL;
  }
  free(buf);
}

const char *pl_sub_at(uint64_t ms) {
  for (int i = 0; i < ncues; i++)
    if (ms >= cues[i].start && ms <= cues[i].end) return cues[i].text;
  return NULL;
}
