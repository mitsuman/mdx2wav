
cmake_minimum_required(VERSION 3.15)

project(mdx2wav)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_RELEASE "-Wall -O3")
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -O3")
set(CMAKE_C_FLAGS_DEBUG "-Wall -g")
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -g")

set(MDX2WAV_SOURCES
	src/mdx2wav.cpp
	src/command_line_parser.cpp
	src/zmd/zmd_player.cpp
	src/zmd/msm6258.cpp
	gamdx/downsample/downsample.cpp
	gamdx/fmgen/fmgen.cpp
	gamdx/fmgen/fmtimer.cpp
	gamdx/fmgen/opm.cpp
	gamdx/mxdrvg/so.cpp
	gamdx/mxdrvg/opm_delegate.cpp
	gamdx/pcm8/pcm8.cpp
	gamdx/pcm8/x68pcm8.cpp
	gamdx/mame/ym2151.c
	src/spc/spc_player.cpp
	src/spc/spc_resampler.cpp
	gamdx/snes_spc/SNES_SPC.cpp
	gamdx/snes_spc/SNES_SPC_misc.cpp
	gamdx/snes_spc/SNES_SPC_state.cpp
	gamdx/snes_spc/SPC_DSP.cpp
	gamdx/snes_spc/SPC_Filter.cpp
	src/psf/psf_player.cpp
	gamdx/psf/psflib.c
	gamdx/psf/he/bios.c
	gamdx/psf/he/iop.c
	gamdx/psf/he/ioptimer.c
	gamdx/psf/he/psx.c
	gamdx/psf/he/r3000.c
	gamdx/psf/he/spu.c
	gamdx/psf/he/spucore.c
	gamdx/psf/he/vfs.c
	src/vgm/vgm_player.cpp
	src/vgm/libvgm_backend.cpp
	src/vgm/sn76489.cpp
	gamdx/opn2/ym3438.c
)

if(APPLE)
	list(APPEND MDX2WAV_SOURCES src/platform_mac.cpp)
endif()

# SDL2 Visualizer support
option(ENABLE_VISUALIZER "Enable SDL2 visualizer" ON)

if(ENABLE_VISUALIZER)
	find_package(SDL2 REQUIRED)
	find_package(SDL2_ttf REQUIRED)
	find_package(SDL2_image REQUIRED)
	
	# FFmpeg for video encoding
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
	
	list(APPEND MDX2WAV_SOURCES
		src/visualizer/visualizer.cpp
		src/visualizer/visualizer_common.cpp
		src/visualizer/visualizer_adpcm.cpp
		src/visualizer/ym2151_state.cpp
		src/visualizer/video_encoder.cpp
		src/visualizer/spectrum_analyzer.cpp
		src/visualizer/visualizer_spc.cpp
		src/spc/spc_visualizer.cpp
		gamdx/mxdrvg/opm_visualizer.cpp
		gamdx/mxdrvg/pcm8_visualizer.cpp
	)
	
	include_directories(${SDL2_INCLUDE_DIRS})
	include_directories(/opt/homebrew/include/SDL2)
	include_directories(${FFMPEG_INCLUDE_DIRS})
	add_definitions(-DENABLE_VISUALIZER)
endif()

# The PSF core is plain C and is compiled as such.
set_source_files_properties(
	gamdx/psf/psflib.c
	gamdx/psf/he/bios.c
	gamdx/psf/he/iop.c
	gamdx/psf/he/ioptimer.c
	gamdx/psf/he/psx.c
	gamdx/psf/he/r3000.c
	gamdx/psf/he/spu.c
	gamdx/psf/he/spucore.c
	gamdx/psf/he/vfs.c
	gamdx/opn2/ym3438.c
	PROPERTIES LANGUAGE C)

add_executable(mdx2wav ${MDX2WAV_SOURCES})
target_compile_definitions(mdx2wav PRIVATE EMU_COMPILE EMU_LITTLE_ENDIAN)
target_include_directories(mdx2wav PRIVATE gamdx/psf gamdx/psf/he)
# libvgm for accurate VGM playback (player + sound emulation cores).
set(BUILD_LIBAUDIO OFF CACHE BOOL "" FORCE)
# Only the chips VGM playback needs (YM2612 + SN76489); building every core
# would also clash with the YM2151 we already ship.
set(SNDEMU__ALL OFF CACHE BOOL "" FORCE)
set(SNDEMU_YM2612_NUKED ON CACHE BOOL "" FORCE)
set(SNDEMU_SN76496_MAME ON CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libvgm EXCLUDE_FROM_ALL)
target_link_libraries(mdx2wav vgm-player vgm-emu vgm-utils)

find_package(ZLIB REQUIRED)
target_link_libraries(mdx2wav ZLIB::ZLIB)

if(APPLE)
	target_link_libraries(mdx2wav "-framework AudioToolbox")
	if(ENABLE_VISUALIZER)
		target_link_libraries(mdx2wav "-framework CoreMIDI" "-framework CoreFoundation")
	endif()
endif()

if(ENABLE_VISUALIZER)
	target_link_libraries(mdx2wav ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf SDL2_image::SDL2_image)
	
	# FFmpegライブラリを正しくリンク
	target_include_directories(mdx2wav PRIVATE ${FFMPEG_INCLUDE_DIRS})
	target_link_directories(mdx2wav PRIVATE ${FFMPEG_LIBRARY_DIRS})
	target_link_libraries(mdx2wav ${FFMPEG_LDFLAGS})
	
	# iconv for Shift_JIS to UTF-8 conversion
	if(APPLE)
		target_link_libraries(mdx2wav iconv)
	endif()
endif()
