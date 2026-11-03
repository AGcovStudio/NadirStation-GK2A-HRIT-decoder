cmake_minimum_required(VERSION 3.21)
project(NadirStation_GK2A_HRIT_Decoder VERSION 1.0.4 LANGUAGES C CXX)

# MinGW Makefiles are single-configuration generators: default to Release so the per-sample DSP is optimised.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# ---- third-party sources (OpenJPEG, libjpeg-turbo) --------------------------------------------------------------
# Either put copies in ./third_party, or leave them in the NadirStation tree next to this folder.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openjpeg-2.5.3")
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openjpeg-2.5.3")
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")      # this folder sits inside the NadirStation tree
else()
    set(_default_tp "${CMAKE_CURRENT_SOURCE_DIR}/../fy2_groundstation/third_party")
endif()
set(HRIT_THIRD_PARTY_DIR "${_default_tp}" CACHE PATH "Folder holding openjpeg-2.5.3 and libjpeg-turbo-3.1.2")
get_filename_component(HRIT_TP "${HRIT_THIRD_PARTY_DIR}" ABSOLUTE)
foreach(_d openjpeg-2.5.3 libjpeg-turbo-3.1.2)
    if(NOT EXISTS "${HRIT_TP}/${_d}/CMakeLists.txt")
        message(FATAL_ERROR "Missing ${HRIT_TP}/${_d}. Set -DHRIT_THIRD_PARTY_DIR=<folder containing ${_d}>.")
    endif()
endforeach()

if(MSVC)
    set(HRIT_WARN /W4 /permissive-)
else()
    set(HRIT_WARN -Wall -Wextra)
endif()

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

# ---- Qt-free receive chain: reader + demodulator + framing + LRIT file assembly ---------------------------------
add_library(hrit_core STATIC
    src/iqreader.cpp src/pipeline.cpp
    src/dsp.cpp src/gk2adecoder.cpp src/gk2adatalink.cpp src/gk2ahrit.cpp)
target_include_directories(hrit_core PUBLIC src)
target_compile_options(hrit_core PRIVATE ${HRIT_WARN})
find_package(Threads REQUIRED)
target_link_libraries(hrit_core PUBLIC Threads::Threads)

# ---- HRIT image library (JPEG2000 / JPEG segments -> pictures, DES decryption) -----------------------------------
add_library(hrit_images STATIC src/gk2ahritimages.cpp src/des.cpp)
target_include_directories(hrit_images PUBLIC src PRIVATE
    "${HRIT_TP}/openjpeg-2.5.3/src/lib/openjp2"
    "${CMAKE_BINARY_DIR}/openjpeg/src/lib/openjp2"
    "${HRIT_TP}/libjpeg-turbo-3.1.2/src"
    "${HRIT_JPEG_BINARY}")
target_compile_definitions(hrit_images PRIVATE OPJ_STATIC)
target_link_libraries(hrit_images PUBLIC hrit_core Qt6::Core Qt6::Gui openjp2 nadir_turbojpeg)

# ---- the application ---------------------------------------------------------------------------------------------
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

# Command-line check of the receive chain (no Qt GUI): HritCli <recording> [manual-rate-Hz]
add_executable(HritCli tests/test_pipeline.cpp)
target_link_libraries(HritCli PRIVATE hrit_core)
set_target_properties(HritCli PROPERTIES OUTPUT_NAME hrit_cli)

# The coastline overlay reads these two files next to the program.
add_custom_command(TARGET HritDecoder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:HritDecoder>/resources/maps"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/maps/landmask.jpg"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/maps/ne_10m_admin_0_countries_chn.geojson"
        "$<TARGET_FILE_DIR:HritDecoder>/resources/maps")

install(TARGETS HritDecoder RUNTIME DESTINATION .)
install(DIRECTORY resources/maps DESTINATION resources)
