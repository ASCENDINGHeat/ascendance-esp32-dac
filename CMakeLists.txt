# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Enable TinyUSB Audio Class Driver for UAC 1.0 USB DAC
add_compile_definitions(
    CFG_TUD_AUDIO=1
    CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE=48000
    CFG_TUD_AUDIO_ENABLE_EP_OUT=1
    CFG_TUD_AUDIO_ENABLE_EP_IN=0
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=196
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=2048
    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX=2
    CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX=16
    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX=2
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(ASCENDANCE)
