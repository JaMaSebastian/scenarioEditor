#pragma once

#include "pch.h"
#include "EntityTypeCatalog.h"
#include "SettingsIO.h"

#include <string>

class CScenarioEditorApp : public CWinApp
{
public:
    CScenarioEditorApp();

    BOOL InitInstance() override;

    // Loaded once at startup from EntityTypeCatalog.ini next to the exe.
    // Lifetime equals the application; passed by const-pointer into the
    // Asset / Entity Editor page (§7.5).
    const EntityTypeCatalog& Catalog() const { return m_catalog; }

    // Mutable: the dialog reads on init, updates on destroy.
    Settings&       Settings()       { return m_settings; }
    const ::Settings& Settings() const { return m_settings; }

    // Absolute path to settings.ini next to the executable.
    const std::wstring& SettingsPath() const { return m_settingsPath; }

private:
    EntityTypeCatalog  m_catalog;
    ::Settings         m_settings;
    std::wstring       m_settingsPath;
};

extern CScenarioEditorApp theApp;
