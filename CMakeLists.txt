cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaGitHub C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaGitHub")
set(VITA_TITLEID  "VGHB00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -std=gnu11")

# Root certificates (Mozilla list) used to check GitHub's TLS certificate.
# cacert.pem is kept in the repo. Stop the build if it's missing or broken,
# because the app can't connect without it.
file(SIZE ${CMAKE_SOURCE_DIR}/cacert.pem CACERT_SIZE)
if(CACERT_SIZE LESS 100000)
  message(FATAL_ERROR "cacert.pem is missing or too small (${CACERT_SIZE} bytes)")
endif()

add_executable(${PROJECT_NAME}
  src/main.c
  src/net.c
  src/json.c
)

target_link_libraries(${PROJECT_NAME}
  curl ssl crypto zstd
  vita2d freetype png jpeg z m c pthread
  SceLibKernel_stub SceKernelThreadMgr_stub SceIofilemgr_stub
  SceDisplay_stub SceGxm_stub SceCtrl_stub SceSysmodule_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
  SceNet_stub SceNetCtl_stub SceRtc_stub
)

# UNSAFE = extended permissions, needed to write to uma0:
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE cacert.pem cacert.pem
)
