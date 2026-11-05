#include "img.h"
#include "jf.h"
#include "ui.h"
#include "util.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <stdlib.h>
#include <vita2d.h>

static char net_mem[1024 * 1024];

int main(void) {
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_DIR, 0777);

  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
  SceNetInitParam np = {net_mem, sizeof net_mem, 0};
  sceNetInit(&np);
  sceNetCtlInit();

  vita2d_init();
  jf_init();
  ui_init();

  for (;;) ui_frame();

  return 0;
}
