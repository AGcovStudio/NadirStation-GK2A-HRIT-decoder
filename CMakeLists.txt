cmake_minimum_required(VERSION 3.21)
project(NadirStation_GK2A_HRIT_Decoder VERSION 1.1.0 LANGUAGES C CXX)

# MinGW Makefiles are single-configuration generators: default to Release so the per-sample DSP is optimised.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HRIT_BUILD_GUI "Build the Qt widget application (HritDecoder)" ON)
option(HRIT_BUILD_CLI "Build the command-line receive-chain check (hrit_cli)" ON)
option(HRIT_BUILD_TESTS "Build the unit tests and register them with CTest" ON)
option(HRIT_WITH_ZSTD "Read zstd-compressed ZIQ recordings (needs zstd)" ON)

if(HRIT_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
    qt_standard_project_setup()
endif()

# ---- third-party sources (OpenJPEG, libjpeg-turbo) --------------------------------------------------------------
# Either put copies in ./third_party, or leave them in the NadirStation tree next to this folder.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openjpeg-2.5.3")
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openjpeg-2.5.3")
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")      # this folder sits inside the NadirStation tree
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openjpeg-2.5.3")
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")   # one level deeper, e.g. NadirStation/opensource_release/<this>
else()
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/../fy2_groundstation/third_party")
endif()
# A folder remembered from an earlier configure that no longer holds the sources (the project was moved)
# is forgotten, so the search above applies again.
if(DEFINED CACHE{HRIT_THIRD_PARTY_DIR} AND NOT EXISTS "${HRIT_THIRD_PARTY_DIR}/openjpeg-2.5.3/CMakeLists.txt")
    unset(HRIT_THIRD_PARTY_DIR CACHE)
endif()
set(HRIT_THIRD_PARTY_DIR "${_default_tp}" CACHE PATH "Folder holding openjpeg-2.5.3 and libjpeg-turbo-3.1.2")
get_filename_component(HRIT_TP "${HRIT_THIRD_PARTY_DIR}" ABSOLUTE)

# ---- zstd (needed for zstd-compressed ZIQ files; uncompressed ZIQ works without it) -----------------------------
set(HRIT_ZSTD_TARGET "")
if(HRIT_WITH_ZSTD)
    # 1. A source copy in third_party is preferred, so the build is reproducible and does not depend on
    #    whatever zstd happens to be installed on the machine. (scripts/fetch_third_party.ps1 gets it.)
    set(HRIT_ZSTD_SOURCE_DIR "" CACHE PATH "Optional zstd source tree (contains build/cmake)")
    set(_hrit_zstd_dir "")
    if(HRIT_ZSTD_SOURCE_DIR AND EXISTS "${HRIT_ZSTD_SOURCE_DIR}/build/cmake/CMakeLists.txt")
        set(_hrit_zstd_dir "${HRIT_ZSTD_SOURCE_DIR}")
    else()
        foreach(_d zstd-1.5.7 zstd-1.5.6 zstd)
            if(EXISTS "${HRIT_TP}/${_d}/build/cmake/CMakeLists.txt")
                set(_hrit_zstd_dir "${HRIT_TP}/${_d}")
                break()
            endif()
        endforeach()
    endif()
    if(_hrit_zstd_dir)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
        set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_hrit_zstd_dir}/build/cmake" "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)
        set(HRIT_ZSTD_TARGET libzstd_static)
    endif()
    # 2. A system / package-manager zstd with a CMake config (vcpkg, MSYS2, ...).
    if(NOT HRIT_ZSTD_TARGET)
        find_package(zstd CONFIG QUIET)
        if(TARGET zstd::libzstd_static)
            set(HRIT_ZSTD_TARGET zstd::libzstd_static)
        elseif(TARGET zstd::libzstd_shared)
            set(HRIT_ZSTD_TARGET zstd::libzstd_shared)
        elseif(TARGET zstd::libzstd)
            set(HRIT_ZSTD_TARGET zstd::libzstd)
        endif()
    endif()
    # 3. Header + library without a config file (MSYS2 UCRT64, a hand-built zstd, ...).
    if(NOT HRIT_ZSTD_TARGET)
        find_path(HRIT_ZSTD_INCLUDE_DIR NAMES zstd.h)
        find_library(HRIT_ZSTD_LIBRARY NAMES zstd libzstd)
        if(HRIT_ZSTD_INCLUDE_DIR AND HRIT_ZSTD_LIBRARY)
            add_library(hrit_zstd UNKNOWN IMPORTED)
            set_target_properties(hrit_zstd PROPERTIES
                IMPORTED_LOCATION "${HRIT_ZSTD_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${HRIT_ZSTD_INCLUDE_DIR}")
            set(HRIT_ZSTD_TARGET hrit_zstd)
        endif()
    endif()
    if(NOT HRIT_ZSTD_TARGET)
        message(WARNING "zstd was not found: ZIQ files compressed with zstd cannot be read. "
                        "Run scripts/fetch_third_party.ps1 or pass -DHRIT_WITH_ZSTD=OFF to silence this.")
        set(HRIT_WITH_ZSTD OFF)
    endif()
endif()

if(MSVC)
    set(HRIT_WARN /W4 /permissive-)
else()
    set(HRIT_WARN -Wall -Wextra)
endif()

# ---- Qt-free receive chain: reader + demodulator + framing + LRIT file assembly ---------------------------------
add_library(hrit_core STATIC
    src/iqreader.cpp src/pipeline.cpp src/repair.cpp
    src/dsp.cpp src/gk2adecoder.cpp src/gk2adatalink.cpp src/gk2ahrit.cpp)
target_include_directories(hrit_core PUBLIC src)
target_compile_options(hrit_core PRIVATE ${HRIT_WARN})
find_package(Threads REQUIRED)
target_link_libraries(hrit_core PUBLIC Threads::Threads)
if(HRIT_WITH_ZSTD)
    target_link_libraries(hrit_core PRIVATE ${HRIT_ZSTD_TARGET})
    target_compile_definitions(hrit_core PUBLIC HRIT_HAVE_ZSTD=1)
    message(STATUS "ZIQ (zstd) support: ON")
else()
    message(STATUS "ZIQ (zstd) support: OFF (uncompressed ZIQ is still supported)")
endif()

if(HRIT_BUILD_GUI)
    foreach(_d openjpeg-2.5.3 libjpeg-turbo-3.1.2)
        if(NOT EXISTS "${HRIT_TP}/${_d}/CMakeLists.txt")
            message(FATAL_ERROR "Missing ${HRIT_TP}/${_d}. Set -DHRIT_THIRD_PARTY_DIR=<folder containing ${_d}> "
                                "or run scripts/fetch_third_party.ps1.")
        endif()
    endforeach()

    # Static linking keeps the installed program free of extra image-library DLLs.
    function(hrit_add_openjpeg)
        set(OPENJPEG_NAMESPACE NADIR_OPENJPEG)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        add_subdirectory("${HRIT_TP}/openjpeg-2.5.3" "${CMAKE_BINARY_DIR}/openjpeg" EXCLUDE_FROM_ALL)
    endfunction()
    hrit_add_openjpeg()

    include(ExternalProject)
    set(HRIT_JPEG_BINARY "${CMAKE_BINARY_DIR}/hrit-jpeg")
    ExternalProject_Add(nadir_jpeg_build
        SOURCE_DIR "${HRIT_TP}/libjpeg-turbo-3.1.2"
        BINARY_DIR "${HRIT_JPEG_BINARY}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
            -DWITH_SIMD=OFF -DWITH_TURBOJPEG=ON
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static --parallel 4
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${HRIT_JPEG_BINARY}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
    add_library(nadir_turbojpeg STATIC IMPORTED)
    set_target_properties(nadir_turbojpeg PROPERTIES
        IMPORTED_LOCATION "${HRIT_JPEG_BINARY}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
    add_dependencies(nadir_turbojpeg nadir_jpeg_build)

    # ---- HRIT image library (JPEG2000 / JPEG segments -> pictures, DES decryption) -------------------------------
    add_library(hrit_images STATIC src/gk2ahritimages.cpp src/des.cpp)
    target_include_directories(hrit_images PUBLIC src PRIVATE
        "${HRIT_TP}/openjpeg-2.5.3/src/lib/openjp2"
        "${CMAKE_BINARY_DIR}/openjpeg/src/lib/openjp2"
        "${HRIT_TP}/libjpeg-turbo-3.1.2/src"
        "${HRIT_JPEG_BINARY}")
    target_compile_definitions(hrit_images PRIVATE OPJ_STATIC)
    target_link_libraries(hrit_images PUBLIC hrit_core Qt6::Core Qt6::Gui openjp2 nadir_turbojpeg)

    # ---- the application -----------------------------------------------------------------------------------------
    set(APP_SOURCES
        src/main.cpp src/mainwindow.cpp src/mainwindow.h
        src/decodeworker.cpp src/decodeworker.h
        src/constellationwidget.cpp src/constellationwidget.h
        src/appinfo.h src/qtpath.h
        resources/app.qrc)
    if(WIN32)
        enable_language(RC)
        list(APPEND APP_SOURCES resources/app.rc)
    endif()
    qt_add_executable(HritDecoder WIN32 ${APP_SOURCES})
    set_target_properties(HritDecoder PROPERTIES OUTPUT_NAME NadirStation_GK2A-HRIT_decoder)
    target_compile_options(HritDecoder PRIVATE ${HRIT_WARN})
    target_link_libraries(HritDecoder PRIVATE hrit_images hrit_core Qt6::Core Qt6::Gui Qt6::Widgets)

    # The coastline overlay reads these two files next to the program.
    add_custom_command(TARGET HritDecoder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:HritDecoder>/resources/maps"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/maps/landmask.jpg"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/maps/ne_10m_admin_0_countries_chn.geojson"
            "$<TARGET_FILE_DIR:HritDecoder>/resources/maps")

    install(TARGETS HritDecoder RUNTIME DESTINATION .)
    install(DIRECTORY resources/maps DESTINATION resources)
    install(FILES
        installer/EULA.txt
        installer/THIRD_PARTY_NOTICES.txt
        installer/LGPL_COMPLIANCE.txt
        DESTINATION .)
    install(DIRECTORY installer/LICENSES/ DESTINATION LICENSES)
endif()

if(HRIT_BUILD_CLI)
    # Command-line check of the receive chain (no Qt GUI): hrit_cli <recording> [manual-rate-Hz]
    add_executable(HritCli tests/test_pipeline.cpp)
    target_link_libraries(HritCli PRIVATE hrit_core)
    set_target_properties(HritCli PROPERTIES OUTPUT_NAME hrit_cli)
endif()

if(HRIT_BUILD_TESTS)
    enable_testing()
    add_executable(test_iqreader tests/test_iqreader.cpp)
    target_link_libraries(test_iqreader PRIVATE hrit_core)
    if(HRIT_WITH_ZSTD)
        target_link_libraries(test_iqreader PRIVATE ${HRIT_ZSTD_TARGET})
    endif()
    target_compile_options(test_iqreader PRIVATE ${HRIT_WARN})
    add_test(NAME iqreader COMMAND test_iqreader)

    add_executable(test_dsp tests/test_dsp.cpp)
    target_link_libraries(test_dsp PRIVATE hrit_core)
    target_compile_options(test_dsp PRIVATE ${HRIT_WARN})
    add_test(NAME dsp COMMAND test_dsp)
endif()
