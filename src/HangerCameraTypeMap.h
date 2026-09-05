//=============================================================================
//  HangerCameraTypeMap.h
//-----------------------------------------------------------------------------
//  Maps a scenario entity's DIS type tuple to the camera TYPE KEY that names its
//  presets in Cameras.ini (the <Type> in [CameraViews.<Type>.<Angle>]).
//
//  That key is NOT the catalog subcategory name. It is the Name= field of the
//  matching Entry= line in DISBrowser's Config\Hanger.ini, because that is what
//  DISBrowser's FDISConfigLoader::ResolveCameraTypeKey returns for a live entity
//  and therefore what both HUDs key their saved views by. The two naming schemes
//  genuinely differ — the catalog calls 1.2.50.7 "EGAT" while the hanger entry is
//  "EGOT", and 1.3.0.3 is "Trawler Ship" vs "Trawler" — so guessing from the
//  catalog would mis-resolve exactly the entities most likely to be miscamera'd.
//
//  This mirrors DISBrowser's matching rules deliberately:
//    * an Entry's EntityType is K.D.Ca.Sc (4 parts) or K.D.Ca.Sc.Sp (5 parts);
//    * a 5-part entry matches only when Specific matches too;
//    * entries are searched most-specific-first, so 1.2.1.4.1 (Raptor1) wins over
//      1.2.1.4 (F22) for an entity that carries Specific=1.
//  Keep those rules in step with FDISConfigLoader::LoadHangerCalibrations.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-01
//=============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// HangerCameraTypeMap — the Name/EntityType pairs from Hanger.ini's Entry= lines
//-----------------------------------------------------------------------------
class HangerCameraTypeMap
{
public:
    // Clear and repopulate from an absolute path to Hanger.ini. Returns false on a
    // missing/unreadable file (leaving the map empty); never throws. An empty map is
    // a supported state: callers fall back to showing every preset.
    bool LoadFromIni(const std::wstring& path);

    // The camera type key for this tuple, or "" when no Entry= line matches — which
    // means DISBrowser has no calibration for it and it will have no saved views.
    std::string ResolveTypeKey(uint8_t kind, uint8_t domain, uint8_t category,
                               uint8_t subcategory, uint8_t specific) const;

    bool   Loaded() const { return !m_entries.empty(); }
    size_t Count()  const { return m_entries.size(); }

private:
    struct Entry
    {
        std::string name;
        uint8_t kind = 0, domain = 0, category = 0, subcategory = 0, specific = 0;
        bool    hasSpecific = false;
    };

    std::vector<Entry> m_entries;   // sorted most-specific-first
};
