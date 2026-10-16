cmake_minimum_required(VERSION 3.15)
project(reaper_drop_to_sampler LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(RC)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(reaper_drop_to_sampler SHARED
    src/main.cpp
    src/resource.rc
)

target_include_directories(reaper_drop_to_sampler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(reaper_drop_to_sampler PROPERTIES
    PREFIX ""
    OUTPUT_NAME "reaper_drop_to_sampler"
    SUFFIX ".dll"
)

if(MINGW)
    target_compile_options(reaper_drop_to_sampler PRIVATE -pthread)
    # ReaperPluginEntry is already __declspec(dllexport) — no .def file needed.
    # Static-link MinGW runtime so clean PCs do not need libwinpthread-1.dll etc.
    target_link_libraries(reaper_drop_to_sampler PRIVATE
        -static-libgcc
        -static-libstdc++
        "-Wl,-Bstatic"
        -lstdc++
        -lpthread
        -lwinpthread
        "-Wl,-Bdynamic"
        user32 gdi32 shell32 ole32 gdiplus msimg32
    )
elseif(WIN32)
    target_link_libraries(reaper_drop_to_sampler PRIVATE
        user32 gdi32 shell32 ole32 gdiplus msimg32
    )
endif()
