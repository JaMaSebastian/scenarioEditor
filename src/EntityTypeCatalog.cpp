//=============================================================================
//  EntityTypeCatalog.cpp
//-----------------------------------------------------------------------------
//  Implements EntityTypeCatalog: parsing EntityTypeCatalog.ini into the
//  in-memory hierarchy (including migration of legacy [Airframe.*] sections
//  into the per-Subcategory attribute model), const lookups by numeric ID,
//  the AirframeProfile compatibility shim, and writing the catalog back out.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "EntityTypeCatalog.h"
#include "../log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace
{
    // Convert a UTF-16 INI string to UTF-8 for storage and LOG output.
    std::string Narrow(const std::wstring& s)
    {
        if (s.empty()) return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0,
                                            nullptr, nullptr);
        std::string out(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                              static_cast<int>(s.size()), out.data(), n,
                              nullptr, nullptr);
        return out;
    }

    //
    // ReadKey — read a single key's string value from a section of the INI.
    //   Returns an empty string when the key is absent.
    //
    std::wstring ReadKey(const wchar_t* section, const wchar_t* key,
                         const std::wstring& path)
    {
        wchar_t buf[1024];
        const DWORD n = ::GetPrivateProfileStringW(section, key, L"",
                                                   buf, _countof(buf),
                                                   path.c_str());
        return std::wstring(buf, n);
    }

    // Split "Kind.1" / "Domain.1.2" / "Category.1.2.3" etc. into the lead
    // token plus up to four trailing numeric components. Returns true if
    // the section starts with `prefix` AND has exactly `expectedNumbers`
    // numeric components after the dot.
    bool ParseSection(const wchar_t* section, const wchar_t* prefix,
                      int expectedNumbers, int outNumbers[4])
    {
        const size_t prefixLen = std::wcslen(prefix);
        if (std::wcsncmp(section, prefix, prefixLen) != 0)
            return false;
        if (section[prefixLen] != L'.')
            return false;

        const wchar_t* cursor = section + prefixLen + 1;
        int count = 0;
        while (count < 4)
        {
            wchar_t* end = nullptr;
            const long v = std::wcstol(cursor, &end, 10);
            if (end == cursor)
                return false; // not a number where one was expected
            if (v < 0 || v > 65535)
                return false;
            outNumbers[count++] = static_cast<int>(v);
            if (*end == L'\0')
                return count == expectedNumbers;
            if (*end != L'.')
                return false;
            cursor = end + 1;
        }
        return false; // more than 4 numeric components
    }

    //
    // SortById — stable-order a list of catalog entries by ascending numeric id.
    //
    void SortById(std::vector<CatalogEntry>& v)
    {
        std::sort(v.begin(), v.end(),
                  [](const CatalogEntry& a, const CatalogEntry& b) { return a.id < b.id; });
    }
}

//
// EntityTypeCatalog::Domains — Domains catalogued under a Kind; m_empty if none.
//
const std::vector<CatalogEntry>& EntityTypeCatalog::Domains(uint8_t kindId) const
{
    auto it = m_domainsByKind.find(kindId);
    return (it == m_domainsByKind.end()) ? m_empty : it->second;
}

//
// EntityTypeCatalog::Categories — Categories under a (Kind, Domain); m_empty if none.
//
const std::vector<CatalogEntry>& EntityTypeCatalog::Categories(uint8_t kindId,
                                                                uint8_t domainId) const
{
    auto it = m_categoriesByKindDomain.find({ kindId, domainId });
    return (it == m_categoriesByKindDomain.end()) ? m_empty : it->second;
}

//
// EntityTypeCatalog::Subcategories — Subcategories under a (Kind, Domain,
//   Category); m_empty if none.
//
const std::vector<CatalogEntry>& EntityTypeCatalog::Subcategories(uint8_t kindId,
                                                                  uint8_t domainId,
                                                                  uint8_t categoryId) const
{
    auto it = m_subcategoriesByKindDomainCategory.find({ kindId, domainId, categoryId });
    return (it == m_subcategoriesByKindDomainCategory.end()) ? m_empty : it->second;
}

//
// EntityTypeCatalog::Profile — build the legacy AirframeProfile envelope for a
//   (Kind, Domain, Category, Subcategory) tuple by reading well-known attribute
//   keys off the Subcategory. Returns a profile with valid=false when the tuple
//   is unknown or carries no attribute values.
//
AirframeProfile EntityTypeCatalog::Profile(uint8_t kindId, uint8_t domainId,
                                           uint8_t categoryId, uint8_t subcategoryId) const
{
    // Compatibility shim: look up well-known attribute names in the
    // Subcategory's attribute map and populate the legacy struct fields.
    auto it = m_subcategoriesByKindDomainCategory.find({ kindId, domainId, categoryId });
    if (it == m_subcategoriesByKindDomainCategory.end()) return AirframeProfile{};
    const CatalogEntry* sub = nullptr;
    for (const auto& e : it->second)
        if (e.id == subcategoryId) { sub = &e; break; }
    if (!sub || sub->attributeValues.empty()) return AirframeProfile{};

    auto getD = [&](const char* key) -> double
    {
        auto kv = sub->attributeValues.find(key);
        if (kv == sub->attributeValues.end() || kv->second.empty()) return 0.0;
        char* end = nullptr;
        const double d = std::strtod(kv->second.c_str(), &end);
        return (end == kv->second.c_str()) ? 0.0 : d;
    };

    AirframeProfile p;
    p.valid               = true;
    p.name                = sub->name;
    p.lengthM             = getD("LengthMeters");
    p.wingspanM           = getD("WingspanMeters");
    // Ships are catalogued with a beam instead of a wingspan; it is the same
    // across-track width to everything that sizes an entity (camera fit,
    // explosion footprint), so fall back to it.
    if (p.wingspanM <= 0.0) p.wingspanM = getD("BeamMeters");
    p.heightM             = getD("HeightMeters");
    p.maxSpeedMps         = getD("MaxSpeedMetersPerSecond");
    p.cruiseSpeedMps      = getD("CruiseSpeedMetersPerSecond");
    p.stallSpeedMps       = getD("StallSpeedMetersPerSecond");
    p.neverExceedMps      = getD("NeverExceedSpeedMetersPerSecond");
    p.maxTaxiMps          = getD("MaxTaxiSpeedMetersPerSecond");
    p.serviceCeilingM     = getD("ServiceCeilingMeters");
    p.maxAltM             = getD("MaxAltitudeMeters");
    p.minAltM             = getD("MinAltitudeMeters");
    p.maxClimbRateMps     = getD("MaxClimbRateMetersPerSecond");
    p.normalClimbRateMps  = getD("NormalClimbRateMetersPerSecond");
    p.maxDescentRateMps   = getD("MaxDescentRateMetersPerSecond");
    p.normalDescentRateMps= getD("NormalDescentRateMetersPerSecond");
    p.maxBankDeg          = getD("MaxBankAngleDeg");
    p.normalBankDeg       = getD("NormalBankAngleDeg");
    p.maxTurnRateDps      = getD("MaxTurnRateDegPerSec");
    p.maxPitchDeg         = getD("MaxPitchAngleDeg");
    p.maxRollRateDps      = getD("MaxRollRateDegPerSec");
    p.maxG                = getD("MaxG");
    p.maxAccelMps2        = getD("MaxAccelMetersPerSecond2");
    p.maxDecelMps2        = getD("MaxDecelMetersPerSecond2");
    return p;
}

//
// EntityTypeCatalog::Clear — wipe all in-memory hierarchy containers.
//
void EntityTypeCatalog::Clear()
{
    m_kinds.clear();
    m_domainsByKind.clear();
    m_countries.clear();
    m_categoriesByKindDomain.clear();
    m_subcategoriesByKindDomainCategory.clear();
}

//
// EntityTypeCatalog::SaveToIni — serialize the catalog to disk in the current
//   [Category.*]/[Subcategory.*] attribute format (no legacy [Airframe.*]).
//   Deletes any existing file first to avoid stale sections. Returns true.
//
bool EntityTypeCatalog::SaveToIni(const std::wstring& path) const
{
    // Delete the existing file so we can write fresh (avoids stale
    // sections/keys from a smaller-than-current catalog). Then use
    // WritePrivateProfileString to write each section.
    ::DeleteFileW(path.c_str());

    auto WriteStr = [&path](const wchar_t* sec, const wchar_t* key, const std::wstring& val)
    {
        ::WritePrivateProfileStringW(sec, key, val.c_str(), path.c_str());
    };
    auto Widen = [](const std::string& s) -> std::wstring
    {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                              static_cast<int>(s.size()), w.data(), n);
        return w;
    };

    // [Format]
    WriteStr(L"Format", L"Version", L"1");
    WriteStr(L"Format", L"Source",
             L"SISO-REF-010 subset (ScenarioEditor Catalog Editor)");

    // [Kind.N]
    for (const auto& k : m_kinds)
    {
        wchar_t sec[32]; std::swprintf(sec, 32, L"Kind.%u", k.id);
        WriteStr(sec, L"Name", Widen(k.name));
    }
    // [Domain.K.D]
    for (const auto& [kid, doms] : m_domainsByKind)
        for (const auto& d : doms)
        {
            wchar_t sec[64]; std::swprintf(sec, 64, L"Domain.%u.%u", kid, d.id);
            WriteStr(sec, L"Name", Widen(d.name));
        }
    // [Country.N]
    for (const auto& c : m_countries)
    {
        wchar_t sec[32]; std::swprintf(sec, 32, L"Country.%u", c.id);
        WriteStr(sec, L"Name", Widen(c.name));
    }
    // [Category.K.D.C] — Name + Attribute.N list
    for (const auto& [kd, cats] : m_categoriesByKindDomain)
        for (const auto& c : cats)
        {
            wchar_t sec[64]; std::swprintf(sec, 64, L"Category.%u.%u.%u", kd.first, kd.second, c.id);
            WriteStr(sec, L"Name", Widen(c.name));
            for (size_t i = 0; i < c.attributeNames.size(); ++i)
            {
                wchar_t key[32]; std::swprintf(key, 32, L"Attribute.%zu", i + 1);
                WriteStr(sec, key, Widen(c.attributeNames[i]));
            }
        }
    // [Subcategory.K.D.C.SC] — Name + attribute values (in the order
    // the parent Category lists them, so the INI is human-readable).
    for (const auto& [kdc, subs] : m_subcategoriesByKindDomainCategory)
    {
        const uint8_t k = std::get<0>(kdc);
        const uint8_t d = std::get<1>(kdc);
        const uint8_t c = std::get<2>(kdc);
        // Resolve the parent Category for the attribute order.
        const std::vector<std::string>* order = nullptr;
        auto catIt = m_categoriesByKindDomain.find({ k, d });
        if (catIt != m_categoriesByKindDomain.end())
            for (const auto& ce : catIt->second)
                if (ce.id == c) { order = &ce.attributeNames; break; }

        for (const auto& s : subs)
        {
            wchar_t sec[64]; std::swprintf(sec, 64, L"Subcategory.%u.%u.%u.%u", k, d, c, s.id);
            WriteStr(sec, L"Name", Widen(s.name));
            if (order)
            {
                for (const auto& key : *order)
                {
                    auto kv = s.attributeValues.find(key);
                    const std::string val = (kv == s.attributeValues.end()) ? "" : kv->second;
                    WriteStr(sec, std::wstring(Widen(key)).c_str(), Widen(val));
                }
            }
            else
            {
                // Fallback (no parent Category): write attributes in
                // whatever order the map iterates.
                for (const auto& kv : s.attributeValues)
                    WriteStr(sec, Widen(kv.first).c_str(), Widen(kv.second));
            }
        }
    }

    // Flush (Windows caches profile writes).
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

//
// EntityTypeCatalog::LoadFromIni — clear and repopulate the catalog from an INI
//   file. Enumerates every section, dispatches by prefix (Kind/Domain/Country/
//   Category/Subcategory), migrates legacy [Airframe.*] sections into the
//   Category schema + Subcategory attribute maps, sorts each list by id, and
//   logs a summary. Returns Loaded() (false on missing file/empty result).
//
bool EntityTypeCatalog::LoadFromIni(const std::wstring& path)
{
    m_kinds.clear();
    m_domainsByKind.clear();
    m_countries.clear();
    m_categoriesByKindDomain.clear();
    m_subcategoriesByKindDomainCategory.clear();

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        sprintf_s(szError, sizeof(szError),
                  "EntityTypeCatalog: file not found, dropdowns will be empty");
        LOG(szError);
        return false;
    }

    // Pull the full list of sections; double-null-terminated string.
    std::vector<wchar_t> namesBuf(32768);
    DWORD nameLen = 0;
    for (;;)
    {
        nameLen = ::GetPrivateProfileSectionNamesW(namesBuf.data(),
                                                   static_cast<DWORD>(namesBuf.size()),
                                                   path.c_str());
        // WPP returns a length one short of the buffer when truncated.
        if (nameLen + 2 < namesBuf.size())
            break;
        namesBuf.resize(namesBuf.size() * 2);
        if (namesBuf.size() > (1 << 20)) // 1 MB sanity cap
            break;
    }

    int kindCount = 0, domainCount = 0, countryCount = 0,
        categoryCount = 0, subcategoryCount = 0, airframeCount = 0, skipped = 0;

    // Read all keys from a section as raw UTF-16 chunks. Returns
    // [key, value] pairs in source order.
    auto ReadAllKeys = [&path](const wchar_t* sec) -> std::vector<std::pair<std::wstring, std::wstring>>
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::vector<wchar_t> buf(8192);
        DWORD got = 0;
        for (;;)
        {
            got = ::GetPrivateProfileSectionW(sec, buf.data(),
                                              static_cast<DWORD>(buf.size()),
                                              path.c_str());
            if (got + 2 < buf.size()) break;
            buf.resize(buf.size() * 2);
            if (buf.size() > (1 << 18)) break;
        }
        for (DWORD i = 0; i < got; )
        {
            const wchar_t* line = buf.data() + i;
            const size_t len = std::wcslen(line);
            i += static_cast<DWORD>(len + 1);
            if (len == 0) continue;
            const wchar_t* eq = std::wcschr(line, L'=');
            if (!eq) continue;
            out.emplace_back(std::wstring(line, eq), std::wstring(eq + 1));
        }
        return out;
    };

    for (DWORD i = 0; i < nameLen; )
    {
        const wchar_t* sec = namesBuf.data() + i;
        const size_t len = std::wcslen(sec);
        i += static_cast<DWORD>(len + 1);
        if (len == 0) continue;

        // Legacy [Airframe.k.d.c.sc] sections — migrate to the new
        // per-Subcategory attribute model. Every non-Name key becomes
        // an attribute value on Subcategory (k,d,c,sc) AND its name is
        // appended to Category (k,d,c)'s schema (if not already present).
        // Subcategory + Category sections always appear earlier in the
        // INI per our canonical ordering, so lookup is reliable.
        {
            int afNum[4] = {};
            if (ParseSection(sec, L"Airframe", 4, afNum))
            {
                const uint8_t k = static_cast<uint8_t>(afNum[0]);
                const uint8_t d = static_cast<uint8_t>(afNum[1]);
                const uint8_t c = static_cast<uint8_t>(afNum[2]);
                const uint16_t sc = static_cast<uint16_t>(afNum[3]);

                // Locate the parent Category and the Subcategory.
                CatalogEntry* catEntry = nullptr;
                auto catIt = m_categoriesByKindDomain.find({ k, d });
                if (catIt != m_categoriesByKindDomain.end())
                    for (auto& ce : catIt->second)
                        if (ce.id == c) { catEntry = &ce; break; }

                CatalogEntry* subEntry = nullptr;
                auto subIt = m_subcategoriesByKindDomainCategory.find({ k, d, c });
                if (subIt != m_subcategoriesByKindDomainCategory.end())
                    for (auto& se : subIt->second)
                        if (se.id == sc) { subEntry = &se; break; }

                if (catEntry && subEntry)
                {
                    for (const auto& kv : ReadAllKeys(sec))
                    {
                        const std::string key = Narrow(kv.first);
                        if (key == "Name") continue;
                        // Append to Category schema if not already there.
                        bool already = false;
                        for (const auto& n : catEntry->attributeNames)
                            if (n == key) { already = true; break; }
                        if (!already) catEntry->attributeNames.push_back(key);
                        // Store the value on the Subcategory.
                        subEntry->attributeValues[key] = Narrow(kv.second);
                    }
                }
                ++airframeCount;
                continue;
            }
        }

        const std::wstring name = ReadKey(sec, L"Name", path);
        if (name.empty())
        {
            // [Format] has no Name= key, skip silently. Everything else
            // without a Name= is malformed but we don't add it.
            if (std::wcsncmp(sec, L"Format", 6) != 0)
            {
                sprintf_s(szError, sizeof(szError),
                          "EntityTypeCatalog: section [%s] missing Name=, skipping",
                          Narrow(sec).c_str());
                LOG(szError);
                ++skipped;
            }
            continue;
        }

        int numbers[4] = {};
        const std::string narrowName = Narrow(name);

        if (ParseSection(sec, L"Kind", 1, numbers))
        {
            m_kinds.push_back({ static_cast<uint16_t>(numbers[0]), narrowName });
            ++kindCount;
        }
        else if (ParseSection(sec, L"Domain", 2, numbers))
        {
            m_domainsByKind[static_cast<uint8_t>(numbers[0])]
                .push_back({ static_cast<uint16_t>(numbers[1]), narrowName });
            ++domainCount;
        }
        else if (ParseSection(sec, L"Country", 1, numbers))
        {
            m_countries.push_back({ static_cast<uint16_t>(numbers[0]), narrowName });
            ++countryCount;
        }
        else if (ParseSection(sec, L"Category", 3, numbers))
        {
            CatalogEntry e;
            e.id   = static_cast<uint16_t>(numbers[2]);
            e.name = narrowName;
            // Pick up explicit schema entries: Attribute.1, Attribute.2, ...
            // (skip in-order; missing numbers terminate the scan).
            for (int n = 1; n < 1000; ++n)
            {
                wchar_t key[32];
                std::swprintf(key, 32, L"Attribute.%d", n);
                const std::wstring v = ReadKey(sec, key, path);
                if (v.empty()) break;
                e.attributeNames.push_back(Narrow(v));
            }
            m_categoriesByKindDomain[{ static_cast<uint8_t>(numbers[0]),
                                       static_cast<uint8_t>(numbers[1]) }]
                .push_back(std::move(e));
            ++categoryCount;
        }
        else if (ParseSection(sec, L"Subcategory", 4, numbers))
        {
            const uint8_t k = static_cast<uint8_t>(numbers[0]);
            const uint8_t d = static_cast<uint8_t>(numbers[1]);
            const uint8_t c = static_cast<uint8_t>(numbers[2]);
            CatalogEntry e;
            e.id   = static_cast<uint16_t>(numbers[3]);
            e.name = narrowName;
            // Any key other than "Name" is an attribute value. The
            // parent Category's schema is extended to include any
            // attribute names not already in its list.
            CatalogEntry* catEntry = nullptr;
            auto catIt = m_categoriesByKindDomain.find({ k, d });
            if (catIt != m_categoriesByKindDomain.end())
                for (auto& ce : catIt->second)
                    if (ce.id == c) { catEntry = &ce; break; }
            for (const auto& kv : ReadAllKeys(sec))
            {
                const std::string key = Narrow(kv.first);
                if (key == "Name") continue;
                e.attributeValues[key] = Narrow(kv.second);
                if (catEntry)
                {
                    bool already = false;
                    for (const auto& n : catEntry->attributeNames)
                        if (n == key) { already = true; break; }
                    if (!already) catEntry->attributeNames.push_back(key);
                }
            }
            m_subcategoriesByKindDomainCategory[{ k, d, c }].push_back(std::move(e));
            ++subcategoryCount;
        }
        else if (std::wcsncmp(sec, L"Format", 6) == 0)
        {
            // expected, no-op
        }
        else
        {
            sprintf_s(szError, sizeof(szError),
                      "EntityTypeCatalog: unknown section [%s], ignored",
                      Narrow(sec).c_str());
            LOG(szError);
        }
    }

    SortById(m_kinds);
    for (auto& kv : m_domainsByKind)                       SortById(kv.second);
    SortById(m_countries);
    for (auto& kv : m_categoriesByKindDomain)              SortById(kv.second);
    for (auto& kv : m_subcategoriesByKindDomainCategory)   SortById(kv.second);

    sprintf_s(szError, sizeof(szError),
              "EntityTypeCatalog loaded: %d kinds, %d domains, %d countries, "
              "%d categories, %d subcategories, %d airframes (skipped %d)",
              kindCount, domainCount, countryCount,
              categoryCount, subcategoryCount, airframeCount, skipped);
    LOG(szError);
    return Loaded();
}
