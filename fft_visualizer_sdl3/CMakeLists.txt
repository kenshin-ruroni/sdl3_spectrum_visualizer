

  cmake_minimum_required(VERSION 3.30)


project(fft_visualizer VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if (MSVC)
    set(CMAKE_CXX_STANDARD 23)
endif()



if (LINUX)
        set(CMAKE_CXX_STANDARD 26)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

#if(MSVC)
#    add_compile_options("/fp:fast")
#  else()
#    add_compile_options("-ffast-math" "-fno-finite-math-only" "-fsigned-zeros" "-fno-associative-math")
#endif()



set(PROJECT_SOURCES
        main.cpp
        miniaudio.c
        spectrogram_renderer.h

        dr_libs/dr_flac.h 
        dr_libs/dr_mp3.h 
        dr_libs/dr_wav.h
)

add_executable(${PROJECT_NAME} main.cpp )

if (MSVC)

    add_compile_options( /arch:AVX2 /arch:SSE2)

    #target_link_options(pitch_control_daw_qt PRIVATE "/NODEFAULTLIB:library") 
    #target_link_directories(pitch_control_daw_qt PRIVATE ${CMAKE_SOURCE_DIR}/SDL3/lib)
    target_link_libraries(${PROJECT_NAME} PRIVATE  ${CMAKE_SOURCE_DIR}/SDL3/lib/SDL3.lib) # SDL3

endif()

if(LINUX)

        add_compile_options(-msse2 -lpthread -lm -march=native -mfma  )
        target_include_directories(${PROJECT_NAME} PUBLIC  /usr/local/include )  
        target_link_directories(${PROJECT_NAME} PUBLIC . )
        target_link_libraries( ${PROJECT_NAME} PRIVATE  ${CMAKE_SOURCE_DIR}/libSDL3.a ) # SDL3

endif()


