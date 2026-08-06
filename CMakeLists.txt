cmake_minimum_required(VERSION 3.22)

project(ScenarioEditor LANGUAGES CXX)

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
    src/CameraZoomMenu.cpp
    src/CameraTargetExtents.cpp
    src/ScenarioCameraIO.cpp
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
    src/ScenarioIO.cpp
    src/EntityTypeCatalog.cpp
    src/CoordTransforms.cpp
    src/ScenarioWorker.cpp
    src/MotionSampler.cpp
    src/SpeedSeed.cpp
    src/DisrecWriter.cpp
    src/DisrecReader.cpp
    src/SettingsIO.cpp
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
