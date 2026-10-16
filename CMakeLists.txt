cmake_minimum_required(VERSION 3.15)

# macOS defaults (must be set before project(); ignored elsewhere)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures (universal)")

project(reaper_drop_to_sampler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(WDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/WDL/WDL)

if(WIN32)
    # ------------------------------------------------------------------ Windows
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${SRC}")

    add_library(reaper_drop_to_sampler SHARED
        src/core.cpp
        src/win/platform_win.cpp
        src/resource.rc
    )
    target_include_directories(reaper_drop_to_sampler PRIVATE ${SRC})
    set_target_properties(reaper_drop_to_sampler PROPERTIES
        PREFIX ""
        OUTPUT_NAME "reaper_drop_to_sampler"
        SUFFIX ".dll"
    )

    set(WIN_LIBS user32 gdi32 shell32 ole32 gdiplus msimg32)
    if(MINGW)
        target_compile_options(reaper_drop_to_sampler PRIVATE -pthread)
        # ReaperPluginEntry is already __declspec(dllexport) — no .def file needed.
        # Static-link MinGW runtime so clean PCs do not need libwinpthread-1.dll etc.
        # -static: libgcc, libstdc++ and libwinpthread are linked in; system DLLs stay dynamic.
        target_link_options(reaper_drop_to_sampler PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_libraries(reaper_drop_to_sampler PRIVATE ${WIN_LIBS})
    else()
        target_compile_definitions(reaper_drop_to_sampler PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(reaper_drop_to_sampler PRIVATE ${WIN_LIBS})
    endif()

elseif(APPLE)
    # -------------------------------------------------------------------- macOS
    # SWELL is provided by REAPER at runtime; swell-modstub.mm binds to it.
    enable_language(OBJCXX)
    if(NOT EXISTS ${WDL_DIR}/swell/swell-modstub.mm)
        message(FATAL_ERROR "WDL not found. Run: git submodule update --init")
    endif()

    add_library(reaper_drop_to_sampler SHARED
        src/core.cpp
        src/mac/platform_mac.mm
        src/mac/mac_resources.mm
        ${WDL_DIR}/swell/swell-modstub.mm
    )
    # reaper_plugin.h includes "../WDL/swell/swell.h": resolve it relative to vendor/WDL/WDL
    target_include_directories(reaper_drop_to_sampler PRIVATE ${SRC} ${WDL_DIR})
    target_compile_definitions(reaper_drop_to_sampler PRIVATE SWELL_PROVIDED_BY_APP)
    target_compile_options(reaper_drop_to_sampler PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
        -Wno-deprecated-declarations
    )
    # swell-modstub.mm uses manual retain/release
    set_source_files_properties(${WDL_DIR}/swell/swell-modstub.mm PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")

    set_target_properties(reaper_drop_to_sampler PROPERTIES
        PREFIX ""
        OUTPUT_NAME "reaper_drop_to_sampler"
        SUFFIX ".dylib"
    )
    target_link_libraries(reaper_drop_to_sampler PRIVATE "-framework Cocoa" "-framework Carbon")

else()
    message(FATAL_ERROR "Only Windows and macOS are supported")
endif()
