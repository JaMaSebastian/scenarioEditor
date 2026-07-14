//=============================================================================
//  PlaysCatalog.h
//-----------------------------------------------------------------------------
//  In-memory model for the "Plays" tab: an editable, INI-persisted catalog of
//  play categories and their subcategories, each subcategory optionally bound
//  to a scenario .ini. Declares PlaySub, PlayCategory, and the PlaysCatalog
//  container with seed/load/save support.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#pragma once

#include "pch.h"

#include <string>
#include <vector>

// Editable catalog backing the "Plays" tab accordion. Each category becomes a
// black header button; each subcategory becomes an indented push button under
// it. A subcategory may be associated with a scenario .ini — clicking it loads
// (and, if RunScenario is set, runs) that scenario. Persisted to plays.ini next
// to the exe, edited via CPlaysEditorDialog. Mirrors the EntityTypeCatalog
// deep-copy + commit-on-Save pattern.
//
// ID-range caps (Resource.h): at most 10 categories and 40 subcategories total,
// so the accordion's IDC_PLAYS_HEADER_* / IDC_PLAYS_ITEM_* ranges never overflow.
struct PlaySub
{
    CString name;
    CString blurb;          // plain-English meaning shown in the client area
    CString scenarioPath;   // associated scenario .ini (absolute); may be empty
};

//-----------------------------------------------------------------------------
// PlayCategory — one accordion category (black header) and its subcategories.
//-----------------------------------------------------------------------------
struct PlayCategory
{
    CString                name;
    std::vector<PlaySub>   subs;
};

//-----------------------------------------------------------------------------
// PlaysCatalog — the full Plays model plus its global "run scenario" option.
//   Holds every category/subcategory and knows how to seed defaults and
//   round-trip itself through plays.ini (deep-copied and committed on Save).
//-----------------------------------------------------------------------------
class PlaysCatalog
{
public:
    std::vector<PlayCategory> categories;
    bool                      runScenario = false;

    // Total subcategory count across all categories (for the 40-sub cap).
    size_t SubCount() const;

    // Populate with the historical hard-coded Defensive/Offensive/ISR plays.
    void SeedDefaults();

    // Read from / write to an INI file. LoadFromIni returns false if the file
    // is missing or has no category count (caller then seeds + saves).
    bool LoadFromIni(const std::wstring& path);
    bool SaveToIni(const std::wstring& path) const;
};
