# CMake 最低版本号要求
cmake_minimum_required(VERSION 2.8)

# 项目信息
project(libNsVad)

# 添加编译选项(针对所有编译器)
add_compile_options(-fPIC)

# 添加编译参数(针对所有编译器)
add_definitions(-D__STDC_FORMAT_MACROS -DDYNAMIC_ANNOTATIONS_ENABLED=1 -DWEBRTC_POSIX -D_DEBUG -D_FILE_OFFSET_BITS=64)
add_definitions(-fno-exceptions -Wall -Wno-unused-parameter -Wno-missing-field-initializers -pipe -fno-strict-aliasing)

# 头文件的搜索路径
include_directories(. ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/common_audio/signal_processing_library/main/interface ${PROJECT_SOURCE_DIR}/modules/audio_processing/utility)
include_directories(${PROJECT_SOURCE_DIR}/system_wrappers/source/spreadsortlib ${PROJECT_SOURCE_DIR}/system_wrappers/interface)
include_directories(${PROJECT_SOURCE_DIR}/modules/audio_processing/aec/main/interface ${PROJECT_SOURCE_DIR}/modules/audio_processing/agc/main/interface)
include_directories(${PROJECT_SOURCE_DIR}/modules/audio_processing/ns/main/interface ${PROJECT_SOURCE_DIR}/modules/audio_processing/vad/main/interface)

# 动态链接库或静态链接库的搜索路径，相当于gcc的-L参数
link_directories(${PROJECT_SOURCE_DIR}/libs/usr/lib64/)
link_directories(/usr/local/lib)
link_directories(/usr/local/freeswitch/lib/nsvad)

# 查找指定目录下的所有源文件，并将名称保存到 DIR_SRC_FILES 变量中
#aux_source_directory(modules/audio_processing/aec/main/source DIR_SRC_FILES)
aux_source_directory(modules/audio_processing/agc/main/source DIR_SRC_FILES)
aux_source_directory(modules/audio_processing/utility DIR_SRC_FILES)
aux_source_directory(common_audio/signal_processing_library/main/source DIR_SRC_FILES)
#aux_source_directory(system_wrappers/source DIR_SRC_FILES)
aux_source_directory(modules/audio_processing/ns/main/source DIR_SRC_FILES)
aux_source_directory(modules/audio_processing/vad/main/source DIR_SRC_FILES)
aux_source_directory(src DIR_SRC_FILES)

# 生成动态库
add_library(nsvad SHARED ${DIR_SRC_FILES})
# 添加链接库,相同于指定-l参数
target_link_libraries(nsvad ssl)
target_link_libraries(nsvad crypto)
target_link_libraries(nsvad uuid)
target_link_libraries(nsvad m)
target_link_libraries(nsvad rt)
target_link_libraries(nsvad pthread)
target_link_libraries(nsvad asound)
target_link_libraries(nsvad avformat)
target_link_libraries(nsvad avcodec)
target_link_libraries(nsvad swscale)
target_link_libraries(nsvad avutil)
target_link_libraries(nsvad vpx)
target_link_libraries(nsvad v4l2)

# 生成可执行文件
add_executable(vadFile readconfig.c vadFile.c)
target_compile_options(vadFile PRIVATE -std=c99)
target_link_libraries(vadFile sndfile)
target_link_libraries(vadFile nsvad)
