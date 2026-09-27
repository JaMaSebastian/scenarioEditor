cmake_minimum_required(VERSION 3.22)

project(ScenarioEditor LANGUAGES CXX)

# Sources are UTF-8 without a BOM; MSVC would otherwise read them as code page
# 1252 and mangle every non-ASCII string literal (the em dashes in the UI).
if (MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Open-DIS C++ sibling project (../open-dis-cpp-master). Built as a
# subproject; we only consume the DIS v7 target (OpenDIS7) — v6 is
# available but unused in V2.
set(OPENDIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../open-dis-cpp-master")
if(NOT TARGET OpenDIS7)
    set(BUILD_OPENDIS6 OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENDIS7 ON  CACHE BOOL "" FORCE)
    add_subdirectory(${OPENDIS_ROOT} open-dis-cpp.build EXCLUDE_FROM_ALL)
endif()

# Dialog-based MFC app (per spec §5). Sources live under src/; log.cpp lives
# at the repo root and is shared with the DISBrowser sibling project.
add_executable(ScenarioEditor WIN32
    src/pch.cpp
    src/ScenarioEditor.cpp
    src/ScenarioEditorDialog.cpp
    src/ScrollablePage.cpp
    src/HelpAwarePage.cpp
    src/ScenarioSetupPage.cpp
    src/AssetEntityEditorPage.cpp
    src/MotionPathEditorPage.cpp
    src/OutputPlaybackPage.cpp
    src/StartupIniWriter.cpp
    src/AttributesDialog.cpp
    src/PlaysPage.cpp
    src/PlaysCatalog.cpp
    src/PlaysEditorDialog.cpp
    src/DeployPage.cpp
    src/PreviewPage.cpp
    src/PreviewCanvas.cpp
    src/CameraTimeline.cpp
    src/CameraPresetCatalog.cpp
    src/HangerCameraTypeMap.cpp
    src/CameraPresetIO.cpp
    src/CameraPresetCombo.cpp
    src/CameraTargetExtents.cpp
    src/EntityTypePickerDialog.cpp
    src/MapTileService.cpp
    src/CesiumView.cpp
    src/CatalogEditorDialog.cpp
    src/CatalogAddDialog.cpp
    src/EntityCameraDialog.cpp
    src/FoliageDialog.cpp
    src/Direct2DContext.cpp
    src/PduBuilder.cpp
    src/UdpSender.cpp
    src/TcpSender.cpp
    src/CameraChannelProducer.cpp
    src/ProcessSupervisor.cpp
    src/ScenarioIO.cpp
    src/EntityTypeCatalog.cpp
    src/CoordTransforms.cpp
    src/ScenarioWorker.cpp
    src/MotionSampler.cpp
    src/SpeedSeed.cpp
    src/DisrecWriter.cpp
    src/DisrecReader.cpp
    src/SettingsIO.cpp
    src/TerrainEndpoint.cpp
    src/TerrainServiceClient.cpp
    src/Validator.cpp
    log.cpp
    src/ScenarioEditor.rc
)

# Round-trip test for the coordinate transforms (§15). Console app; built
# from a single source file so it stays self-contained and fast to iterate.
add_executable(coord_roundtrip
    tools/coord_roundtrip.cpp
    src/CoordTransforms.cpp
)
target_include_directories(coord_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Round-trip test for ScenarioIO (Save → Load → field-level compare).
# Pulls in log.cpp because ScenarioIO uses LOG() / szError for diagnostics.
add_executable(scenario_io_roundtrip
    tools/scenario_io_roundtrip.cpp
    src/ScenarioIO.cpp
    log.cpp
)
target_include_directories(scenario_io_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(scenario_io_roundtrip PRIVATE Shlwapi)

# Standalone sampler test (Stationary + Line). Pulls CoordTransforms in
# because the Line sampler converts lerped geodetics to ECEF.
add_executable(motion_sampler_test
    tools/motion_sampler_test.cpp
    src/MotionSampler.cpp
    src/CoordTransforms.cpp
)
target_include_directories(motion_sampler_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Disrec writer/reader round-trip
add_executable(disrec_roundtrip
    tools/disrec_roundtrip.cpp
    src/DisrecWriter.cpp
    src/DisrecReader.cpp
    log.cpp
)
target_include_directories(disrec_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Validator unit test (rules + bandwidth estimate).
add_executable(validator_test
    tools/validator_test.cpp
    src/Validator.cpp
    src/EntityTypeCatalog.cpp
    log.cpp
)
target_include_directories(validator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# EntityTypeCatalog.cpp pulls in pch.h → MFC, so this console test needs the
# same MFC-shared/Unicode config as the main app (otherwise afx.h errors on /MD).
target_compile_definitions(validator_test PRIVATE _AFXDLL UNICODE _UNICODE)
set_target_properties(validator_test PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
    VS_GLOBAL_CharacterSet "Unicode"
)

# ProcessSupervisor regression test: exit codes, piped output, and the
# no-orphans / deliberate-survivor guarantees.
add_executable(process_supervisor_test
    tools/process_supervisor_test.cpp
    src/ProcessSupervisor.cpp
    log.cpp
)
target_include_directories(process_supervisor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# ProcessSupervisor.cpp includes pch.h → MFC, so this console test needs the
# same MFC-shared/Unicode config as the main app.
target_compile_definitions(process_supervisor_test PRIVATE _AFXDLL UNICODE _UNICODE)
set_target_properties(process_supervisor_test PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
    VS_GLOBAL_CharacterSet "Unicode"
)

# Mode=Service plumbing: the endpoint resolver, the WinHTTP client, and the
# submit-then-poll loop, checked against a live terrainserver when one is up.
add_executable(terrain_service_test
    tools/terrain_service_test.cpp
    src/TerrainEndpoint.cpp
    src/TerrainServiceClient.cpp
    src/SettingsIO.cpp
    src/StartupIniWriter.cpp
    log.cpp
)
target_include_directories(terrain_service_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# These sources include pch.h -> MFC, so this console test needs the same
# MFC-shared/Unicode config as the app (otherwise afx.h errors on /MD).
target_compile_definitions(terrain_service_test PRIVATE _AFXDLL UNICODE _UNICODE)
set_target_properties(terrain_service_test PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
    VS_GLOBAL_CharacterSet "Unicode"
)
target_link_libraries(terrain_service_test PRIVATE Shlwapi)

# Native terrain tile server — replaces Scripts/wsl/serve.py so the serving
# path needs no WSL and no console window. Plain console app (no MFC, no pch);
# the editor launches it with CREATE_NO_WINDOW.
add_executable(TerrainServer
    tools/terrain_server.cpp
)
target_link_libraries(TerrainServer PRIVATE Ws2_32 Shell32)
set_target_properties(TerrainServer PROPERTIES
    VS_GLOBAL_CharacterSet "MultiByte"
)

target_include_directories(ScenarioEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENDIS_ROOT}/src
)

target_compile_definitions(ScenarioEditor PRIVATE
    _AFXDLL
    WIN32
    _WINDOWS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

target_link_libraries(ScenarioEditor PRIVATE
    Ws2_32
    d2d1
    dwrite
    winmm
    OpenDIS7
)

set_target_properties(ScenarioEditor PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
    VS_GLOBAL_CharacterSet "Unicode"
)

# Unicode for the Win32 / MFC headers.
target_compile_definitions(ScenarioEditor PRIVATE
    UNICODE
    _UNICODE
)

# Precompiled header.
target_precompile_headers(ScenarioEditor PRIVATE src/pch.h)

# OpenDIS7 is a SHARED library, so ScenarioEditor.exe needs OpenDIS7.dll sitting
# next to it to start at all. CMake writes that DLL to the subproject's own
# output directory (open-dis-cpp.build/<config>/), never beside the exe, so it
# has to be copied after every link.
#
# Without this the app only ran because a copy had been placed in Release/ by
# hand once and then carried along by every incremental build since. A clean
# rebuild deletes it, and the failure is invisible at build time: the build
# succeeds, then the exe refuses to start with a bare loader error naming no
# cause. ScenarioEditor is the only target that links OpenDIS7, and every target
# shares one output directory, so this single copy is enough.
add_custom_command(TARGET ScenarioEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenDIS7>
            $<TARGET_FILE_DIR:ScenarioEditor>
    COMMENT "Copying OpenDIS7.dll next to ScenarioEditor.exe"
    VERBATIM
)

# Gimbal-envelope round trip: CameraPresetIO writes, CameraPresetCatalog reads.
# Pins the arc semantics (yaw/roll never reordered, pitch swapped and clamped),
# the surgical-write guarantee, and delete-vs-zero on clear.
add_executable(camera_preset_io_test
    tools/camera_preset_io_test.cpp
    src/CameraPresetIO.cpp
    src/CameraPresetCatalog.cpp
    log.cpp
)
target_include_directories(camera_preset_io_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# Both sources include pch.h → MFC, so this console test needs the same
# MFC-shared/Unicode config as the main app.
target_compile_definitions(camera_preset_io_test PRIVATE _AFXDLL UNICODE _UNICODE)
set_target_properties(camera_preset_io_test PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
    VS_GLOBAL_CharacterSet "Unicode"
)
