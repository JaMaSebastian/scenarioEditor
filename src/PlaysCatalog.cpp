//=============================================================================
//  PlaysCatalog.cpp
//-----------------------------------------------------------------------------
//  Implements the Plays catalog model: default seeding of the historical
//  Defensive/Offensive/ISR plays and load/save of the catalog to a wide INI
//  file (via the Win32 GetPrivateProfile* API), honoring the 10-category /
//  40-subcategory id-range caps.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#include "pch.h"
#include "PlaysCatalog.h"
#include "../log.h"

#include <stdio.h>

namespace
{
    constexpr int kMaxCategories = 10;   // IDC_PLAYS_HEADER_FIRST..LAST span
    constexpr int kMaxSubsTotal  = 40;   // IDC_PLAYS_ITEM_FIRST..LAST span

    // --- Typed INI helpers (wide, section/key/value) ---------------------
    CString ReadStr(const std::wstring& path, LPCWSTR section, LPCWSTR key,
                    LPCWSTR def = L"")
    {
        wchar_t buf[2048] = {};
        ::GetPrivateProfileStringW(section, key, def, buf, _countof(buf),
                                   path.c_str());
        return CString(buf);
    }

    int ReadInt(const std::wstring& path, LPCWSTR section, LPCWSTR key, int def)
    {
        return static_cast<int>(
            ::GetPrivateProfileIntW(section, key, def, path.c_str()));
    }

    bool WriteStr(const std::wstring& path, LPCWSTR section, LPCWSTR key,
                  const CString& value)
    {
        return ::WritePrivateProfileStringW(section, key, value.GetString(),
                                            path.c_str()) != FALSE;
    }

    bool WriteInt(const std::wstring& path, LPCWSTR section, LPCWSTR key, int value)
    {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%d", value);
        return WriteStr(path, section, key, CString(buf));
    }

    bool FileExists(const std::wstring& p)
    {
        const DWORD attrs = ::GetFileAttributesW(p.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES &&
               !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }
}

//
// PlaysCatalog::SubCount — total subcategories across all categories (for caps).
//
size_t PlaysCatalog::SubCount() const
{
    size_t n = 0;
    for (const PlayCategory& c : categories) n += c.subs.size();
    return n;
}

//
// PlaysCatalog::SeedDefaults — replace contents with the built-in
//   Defensive/Offensive/ISR categories and their subcategory blurbs (no
//   scenario bindings); used on first run or when plays.ini is absent.
//
void PlaysCatalog::SeedDefaults()
{
    categories.clear();
    runScenario = false;

    PlayCategory def;
    def.name = _T("Defensive & Security");
    def.subs = {
        { _T("Area Defense"),
          _T("Hold terrain and deny the enemy access to a specific area."), _T("") },
        { _T("Mobile Defense"),
          _T("Allow or shape the enemy's movement, then defeat them with a decisive counterattack."), _T("") },
        { _T("Retrograde"),
          _T("Move away from the enemy in an organized way. This can include delay, withdrawal, or retirement."), _T("") },
        { _T("Screen"),
          _T("Provides early warning. It observes, reports, and may harass, but avoids becoming decisively engaged."), _T("") },
        { _T("Guard"),
          _T("Protects the main force by fighting to gain time, prevent surprise, and stop enemy observation/fire against the main body."), _T("") },
        { _T("Cover"),
          _T("A stronger, more independent security force that operates farther away and can fight to develop the situation before the enemy reaches the main body."), _T("") },
        { _T("Area Security"),
          _T("Protects a specific area, route, facility, population, or activity."), _T("") },
    };
    categories.push_back(def);

    PlayCategory off;
    off.name = _T("Offensive");
    off.subs = {
        { _T("Movement to Contact"),
          _T("Move forward to find and make contact with the enemy when the enemy situation is unclear."), _T("") },
        { _T("Attack"),
          _T("Strike the enemy to destroy/defeat forces or seize terrain."), _T("") },
        { _T("Exploitation"),
          _T("Follow up a successful attack to break the enemy deeper and prevent recovery."), _T("") },
        { _T("Pursuit"),
          _T("Chase and destroy or cut off an enemy force that is trying to escape."), _T("") },
    };
    categories.push_back(off);

    PlayCategory isr;
    isr.name = _T("Intelligence, Surveillance & Reconnaissance (ISR)");
    isr.subs = {
        { _T("Route Reconnaissance"),
          _T("Examine a specific road, trail, waterway, bridge route, or movement path. Used to answer: Can we move through here? Is it blocked? Is it defended?"), _T("") },
        { _T("Zone Reconnaissance"),
          _T("Search an entire zone or corridor from one boundary to another. Used when the commander needs a broad picture of terrain, enemy, obstacles, and routes."), _T("") },
        { _T("Area Reconnaissance"),
          _T("Examine a specific place, such as a town, bridge, hill, airfield, landing zone, port, or suspected enemy position."), _T("") },
        { _T("Reconnaissance in Force"),
          _T("A stronger combat operation meant to make the enemy react, revealing their strength, location, weapons, or intentions. This can involve fighting."), _T("") },
        { _T("Special Reconnaissance"),
          _T("Reconnaissance by special operations forces, often deep, covert, or politically sensitive. Used where normal forces may not be able to go."), _T("") },
        { _T("Surveillance"),
          _T("More passive or continuous observation over time."), _T("") },
        { _T("Screening"),
          _T("Watching an area to give early warning and protect friendly forces."), _T("") },
        { _T("Scouting"),
          _T("Informal or small-unit term for reconnaissance."), _T("") },
        { _T("Patrol"),
          _T("A unit sent out to gather information, secure an area, or fight if needed."), _T("") },
    };
    categories.push_back(isr);
}

//
// PlaysCatalog::LoadFromIni — parse a plays.ini into this catalog.
//   Returns false (leaving the object untouched) when the file is missing,
//   from a newer format version, or has no CategoryCount key, so the caller
//   can fall back to SeedDefaults + save. Clamps to the category/sub caps.
//
bool PlaysCatalog::LoadFromIni(const std::wstring& path)
{
    if (!FileExists(path)) return false;

    // A version guard so a future schema bump can be detected.
    const int version = ReadInt(path, L"Format", L"Version", 0);
    if (version > 1) return false;

    // -1 sentinel distinguishes "no plays defined" (fresh/foreign file) from
    // an intentional zero-category catalog.
    const int catCount = ReadInt(path, L"Plays", L"CategoryCount", -1);
    if (catCount < 0) return false;

    categories.clear();
    runScenario = ReadInt(path, L"Options", L"RunScenario", 0) != 0;

    const int nCats = (catCount > kMaxCategories) ? kMaxCategories : catCount;
    int subsTotal = 0;
    for (int ci = 0; ci < nCats; ++ci)
    {
        wchar_t sect[32] = {};
        swprintf_s(sect, L"Category.%d", ci);

        PlayCategory cat;
        cat.name = ReadStr(path, sect, L"Name");

        const int subCount = ReadInt(path, sect, L"SubCount", 0);
        for (int si = 0; si < subCount && subsTotal < kMaxSubsTotal; ++si)
        {
            wchar_t kName[32], kBlurb[32], kScen[32];
            swprintf_s(kName,  L"Sub%d.Name",     si);
            swprintf_s(kBlurb, L"Sub%d.Blurb",    si);
            swprintf_s(kScen,  L"Sub%d.Scenario", si);

            PlaySub sub;
            sub.name         = ReadStr(path, sect, kName);
            sub.blurb        = ReadStr(path, sect, kBlurb);
            sub.scenarioPath = ReadStr(path, sect, kScen);
            cat.subs.push_back(sub);
            ++subsTotal;
        }
        categories.push_back(cat);
    }
    return true;
}

//
// PlaysCatalog::SaveToIni — write the catalog to plays.ini.
//   Deletes the file first so removed rows don't linger as stale keys, then
//   writes version/options/categories (clamped to caps) and flushes the WPP
//   cache. Returns true if every write succeeded; logs the outcome.
//
bool PlaysCatalog::SaveToIni(const std::wstring& path) const
{
    // Delete first so removed categories/subs don't linger as stale keys.
    ::DeleteFileW(path.c_str());

    bool ok = true;
    ok &= WriteInt(path, L"Format",  L"Version",       1);
    ok &= WriteInt(path, L"Options", L"RunScenario",   runScenario ? 1 : 0);

    const int nCats = (categories.size() > (size_t)kMaxCategories)
                          ? kMaxCategories : (int)categories.size();
    ok &= WriteInt(path, L"Plays", L"CategoryCount", nCats);

    int subsTotal = 0;
    for (int ci = 0; ci < nCats; ++ci)
    {
        const PlayCategory& cat = categories[ci];
        wchar_t sect[32] = {};
        swprintf_s(sect, L"Category.%d", ci);

        ok &= WriteStr(path, sect, L"Name", cat.name);

        int written = 0;
        for (size_t si = 0; si < cat.subs.size() && subsTotal < kMaxSubsTotal; ++si)
        {
            const PlaySub& sub = cat.subs[si];
            wchar_t kName[32], kBlurb[32], kScen[32];
            swprintf_s(kName,  L"Sub%d.Name",     written);
            swprintf_s(kBlurb, L"Sub%d.Blurb",    written);
            swprintf_s(kScen,  L"Sub%d.Scenario", written);
            ok &= WriteStr(path, sect, kName,  sub.name);
            ok &= WriteStr(path, sect, kBlurb, sub.blurb);
            ok &= WriteStr(path, sect, kScen,  sub.scenarioPath);
            ++written;
            ++subsTotal;
        }
        ok &= WriteInt(path, sect, L"SubCount", written);
    }

    // Flush the WPP write cache to disk.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    sprintf_s(szError, sizeof(szError), "PlaysCatalog saved to %S (%s)",
              path.c_str(), ok ? "ok" : "with errors");
    LOG(szError);
    return ok;
}
