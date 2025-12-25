
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
	gamdx/downsample/downsample.cpp
	gamdx/fmgen/fmgen.cpp
	gamdx/fmgen/fmtimer.cpp
	gamdx/fmgen/opm.cpp
	gamdx/mxdrvg/so.cpp
	gamdx/mxdrvg/opm_delegate.cpp
	gamdx/pcm8/pcm8.cpp
	gamdx/pcm8/x68pcm8.cpp
	gamdx/mame/ym2151.c
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
		gamdx/mxdrvg/opm_visualizer.cpp
		gamdx/mxdrvg/pcm8_visualizer.cpp
	)
	
	include_directories(${SDL2_INCLUDE_DIRS})
	include_directories(/opt/homebrew/include/SDL2)
	include_directories(${FFMPEG_INCLUDE_DIRS})
	add_definitions(-DENABLE_VISUALIZER)
endif()

add_executable(mdx2wav ${MDX2WAV_SOURCES})

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
