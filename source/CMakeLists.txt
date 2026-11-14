# =============================================================================
# media_player/source/CMakeLists.txt
#
# 音视频播放器：FFmpeg 软解 → RGB565 → DRM（局部刷新）+ MPP AO 出声
#
# 依赖：
#   prebuilt/      自带 ffmpeg 7.1 静态库（树里没有 ffmpeg 这个 open_source 包）
#   third_party::libdrm   CONFIG_OS_DRM=y 时提供
#   SDK_LIB_DIR      MPP 动态库（.so，板上 /lib 里也有）
# =============================================================================

set(_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt")

add_executable(media_player
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
)

target_include_directories(media_player PRIVATE
    "${_PREBUILT}/include"
)

# SDK MPI 头文件：SDK_INC_DIR 主头 + SDK_CONFIG_INC（含 autoconf.h，缺了编译不过）
if(DEFINED SDK_INC_DIR AND NOT SDK_INC_DIR STREQUAL "")
    target_include_directories(media_player PRIVATE "${SDK_INC_DIR}")
endif()
if(DEFINED SDK_CONFIG_INC AND NOT SDK_CONFIG_INC STREQUAL "")
    target_include_directories(media_player PRIVATE "${SDK_CONFIG_INC}")
endif()
if(DEFINED SDK_LIB_DIR AND NOT SDK_LIB_DIR STREQUAL "")
    target_link_directories(media_player PRIVATE "${SDK_LIB_DIR}")
endif()

target_link_directories(media_player PRIVATE "${_PREBUILT}/lib")

# MPP 音频链路：这些 .so 之间互相隐式依赖，必须整组给出（--start-group 解决顺序）
set(_MPP_LIBS
    ss_mpi_audio ss_mpi_audio_adp ss_mpi ss_mpi_sysbind ss_mpi_sysmem
    ot_osal ot_irq ot_base ot_sys
    aac_comm aac_dec aac_sbr_dec aac_enc aac_sbr_enc
    mp3_dec mp3_enc mp3_lame opus
    voice_engine dnvqe upvqe
    vqe_aec vqe_agc vqe_anr vqe_eq vqe_hpf vqe_hs vqe_record vqe_res
    vqe_talkv2 vqe_common securec
)

target_link_libraries(media_player PRIVATE
    third_party::libdrm
    -Wl,--start-group
        "${_PREBUILT}/lib/libavformat.a"
        "${_PREBUILT}/lib/libavcodec.a"
        "${_PREBUILT}/lib/libswresample.a"
        "${_PREBUILT}/lib/libavutil.a"
        ${_MPP_LIBS}
    -Wl,--end-group
    pthread
    m
)

target_compile_options(media_player PRIVATE -Wall -O2)

install(TARGETS media_player RUNTIME DESTINATION bin)
