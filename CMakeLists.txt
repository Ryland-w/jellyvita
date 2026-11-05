cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaFin C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaFin")
set(VITA_TITLEID  "VFIN00001")
set(VITA_VERSION  "00.10")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -std=gnu11")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/jf.c
  src/img.c
  src/player.c
  src/ui.c
  src/json.c
)

target_link_libraries(${PROJECT_NAME}.elf
  vita2d
  curl ssl crypto z
  freetype png jpeg z m
  pthread
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub
  SceNet_stub SceNetCtl_stub
  SceAvPlayer_stub SceAudio_stub SceAppMgr_stub ScePower_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
