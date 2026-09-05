//=============================================================================
//  ScenarioEditor.h
//-----------------------------------------------------------------------------
//  Declares CScenarioEditorApp, the MFC CWinApp singleton for the Scenario
//  Editor. Owns the process-lifetime data models (entity-type catalog, plays
//  catalog, settings) and their resolved on-disk paths, exposing them to the
//  dialogs via const/mutable accessors.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "EntityTypeCatalog.h"
#include "CameraPresetCatalog.h"
#include "HangerCameraTypeMap.h"
#include "PlaysCatalog.h"
#include "SettingsIO.h"

#include <string>

//-----------------------------------------------------------------------------
// CScenarioEditorApp — the application object (MFC entry point).
//   Loads the catalog, plays, and settings at startup, holds them for the
//   lifetime of the process, and launches the main modal dialog.
//-----------------------------------------------------------------------------
class CScenarioEditorApp : public CWinApp
{
public:
    CScenarioEditorApp();

    //
    // InitInstance — MFC startup hook. Sets per-monitor-v2 DPI awareness,
    // resolves the catalog/settings/plays paths relative to the exe, loads
    // (or seeds) each model, then runs the main dialog modally.
    //
    BOOL InitInstance() override;

    // Loaded once at startup from the canonical source EntityTypeCatalog.ini
    // (the first ancestor of the exe dir that contains it; see InitInstance).
    // Lifetime equals the application; passed by const-pointer into the
    // Asset / Entity Editor page (§7.5).
    const EntityTypeCatalog& Catalog() const { return m_catalog; }
    // Mutable accessor used by the Catalog Editor dialog. The dialog
    // operates on a deep copy and writes back here on Save.
    EntityTypeCatalog& MutableCatalog() { return m_catalog; }

    // Mutable: the dialog reads on init, updates on destroy.
    Settings&       Settings()       { return m_settings; }
    const ::Settings& Settings() const { return m_settings; }

    // Absolute path to settings.ini next to the executable.
    const std::wstring& SettingsPath() const { return m_settingsPath; }
    // Absolute path to the canonical EntityTypeCatalog.ini (resolved to the
    // source copy in InitInstance). The Catalog Editor reads/writes this path.
    const std::wstring& CatalogPath() const { return m_catalogPath; }

    // Editable Plays catalog backing the "Plays" tab accordion. Loaded (or
    // seeded) at startup; edited via the gear-button CPlaysEditorDialog, which
    // operates on a deep copy and writes back here on Save.
    const PlaysCatalog& Plays() const { return m_plays; }
    PlaysCatalog&       MutablePlays() { return m_plays; }
    const std::wstring& PlaysPath() const { return m_playsPath; }

    // Saved camera-view presets loaded from config\Cameras.ini (resolved like the
    // catalog). Read-only; feeds the Preview-tab "Camera" dropdown.
    const CameraPresetCatalog& CameraPresets() const { return m_cameraPresets; }
    const std::wstring& CameraPresetsPath() const { return m_cameraPresetsPath; }

    // Re-read Cameras.ini after the New/Edit Camera dialog has written a gimbal
    // envelope through CameraPresetIO. The catalog is otherwise load-once, so
    // without this the dialog would keep showing the pre-write values for the rest
    // of the session. CAUTION: anything holding an INDEX into Presets() must
    // consume it before calling this.
    bool ReloadCameraPresets() { return m_cameraPresets.LoadFromIni(m_cameraPresetsPath); }

    // DISBrowser project root: the Run tab's folder when set, else the sibling
    // "DISBrowser" beside the exe. Never has a trailing separator, and is not
    // checked for existence — callers that need a real file say so themselves.
    std::wstring DisBrowserProjectDir() const;

    // DIS tuple -> Cameras.ini <Type>, read from DISBrowser's Config\Hanger.ini so a
    // camera dropdown can offer the presets that actually belong to the entity being
    // filmed. Loaded lazily rather than in InitInstance: the DISBrowser project folder
    // is a Run-tab setting the user may not have set yet, and it can change mid-session.
    // Retries while empty, so setting the folder later fixes it without a restart.
    const HangerCameraTypeMap& HangerTypes();

private:
    EntityTypeCatalog  m_catalog;
    CameraPresetCatalog m_cameraPresets;
    HangerCameraTypeMap m_hangerTypes;
    PlaysCatalog       m_plays;
    ::Settings         m_settings;
    std::wstring       m_settingsPath;
    std::wstring       m_catalogPath;
    std::wstring       m_cameraPresetsPath;
    std::wstring       m_playsPath;
};

extern CScenarioEditorApp theApp;
