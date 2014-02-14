
cmake_minimum_required(VERSION 2.8)

project(mdx2wav)

set(CMAKE_C_FLAGS_RELEASE "-Wall -O3")
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -O3")
set(CMAKE_C_FLAGS_DEBUG "-Wall -g")
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -g")

add_executable(mdx2wav
	src/mdx2wav.cpp
	gamdx/downsample/downsample.cpp
	gamdx/fmgen/fmgen.cpp
	gamdx/fmgen/fmtimer.cpp
	gamdx/fmgen/opm.cpp
	gamdx/mxdrvg/so.cpp
	gamdx/mxdrvg/opm_delegate.cpp
	gamdx/pcm8/pcm8.cpp
	gamdx/pcm8/x68pcm8.cpp
)



