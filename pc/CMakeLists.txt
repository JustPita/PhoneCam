cmake_minimum_required(VERSION 3.20)
project(PhoneCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /utf-8 /permissive- /wd4100)

# --- Source de la caméra virtuelle (chargée par le service Frame Server) -------
add_library(PhoneCamSource SHARED
    vcamsource/dllmain.cpp
    vcamsource/mediasource.cpp
    vcamsource/mediastream.cpp
    vcamsource/frames.cpp
    vcamsource/vcamsource.def
)
target_link_libraries(PhoneCamSource PRIVATE mfplat mfuuid mf advapi32)

# --- Application ---------------------------------------------------------------
add_executable(PhoneCam WIN32
    app/main.cpp
    app/net.cpp
    app/decoder.cpp
    app/framewriter.cpp
    app/vcam.cpp
    app/install.cpp
    app/playout.cpp
    app/obs.cpp
    app/app.rc
)
# La DLL est embarquée dans l'exe (ressource RCDATA 2) : un seul fichier à distribuer.
add_dependencies(PhoneCam PhoneCamSource)
target_include_directories(PhoneCam PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(app/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/PhoneCamSource.dll;${CMAKE_CURRENT_SOURCE_DIR}/app/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/app/phonecam.ico")
# Outil de diagnostic : lit quelques images de la webcam « PhoneCam ».
add_executable(camtest tools/camtest.cpp)
# Outil de diagnostic : lit et décode le flux réseau « OBS » (MPEG-TS sur HTTP).
add_executable(tstest tools/tstest.cpp)

# Le manifeste (contrôles communs v6, DPI, UTF-8) est fourni par app.rc.
target_link_options(PhoneCam PRIVATE /MANIFEST:NO)
target_link_libraries(PhoneCam PRIVATE
    ws2_32 iphlpapi mfplat mfuuid mf mfsensorgroup d3d11 dxgi comctl32 ole32 shell32 shlwapi advapi32)
