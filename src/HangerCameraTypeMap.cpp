//=============================================================================
//  HangerCameraTypeMap.cpp
//-----------------------------------------------------------------------------
//  Implements HangerCameraTypeMap: parse DISBrowser's Config\Hanger.ini and
//  resolve a DIS tuple to the camera type key its presets are stored under.
//
//  Hanger.ini keeps every platform on a repeated "Entry=" key inside one
//  [Hanger] section, which the Win32 profile API cannot enumerate, so this reads
//  the file as text and picks the Entry= lines out itself.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-01
//=============================================================================
#include "pch.h"
#include "HangerCameraTypeMap.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{
    // Value of Key="..." (quoted) or Key=token (bare) inside one Entry= line.
    // Quoted form matters: several File= paths contain spaces.
    bool FieldValue(const std::string& line, const char* key, std::string& out)
    {
        const std::string needle = std::string(key) + "=";
        const size_t k = line.find(needle);
        if (k == std::string::npos) return false;
        size_t v = k + needle.size();
        if (v < line.size() && line[v] == '"')
        {
            const size_t end = line.find('"', v + 1);
            if (end == std::string::npos) return false;
            out = line.substr(v + 1, end - v - 1);
            return true;
        }
        size_t end = v;
        while (end < line.size() && !isspace(static_cast<unsigned char>(line[end]))) ++end;
        if (end == v) return false;
        out = line.substr(v, end - v);
        return true;
    }

    // "1.2.50.7" / "1.2.1.4.1" -> the numeric parts. Anything else is rejected;
    // a malformed EntityType simply drops that entry rather than failing the load.
    bool ParseTuple(const std::string& s, std::vector<int>& out)
    {
        out.clear();
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, '.'))
        {
            if (part.empty()) return false;
            for (char c : part) if (!isdigit(static_cast<unsigned char>(c))) return false;
            out.push_back(std::atoi(part.c_str()));
        }
        return out.size() == 4 || out.size() == 5;
    }
}

bool HangerCameraTypeMap::LoadFromIni(const std::wstring& path)
{
    m_entries.clear();

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;

    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first)                                    // strip a UTF-8 BOM
        {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
                                 && static_cast<unsigned char>(line[1]) == 0xBB
                                 && static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
        }

        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == ';') continue;                 // comment
        if (line.compare(b, 6, "Entry=") != 0) continue;

        std::string name, typeStr;
        if (!FieldValue(line, "Name", name) || name.empty()) continue;
        if (!FieldValue(line, "EntityType", typeStr))        continue;

        std::vector<int> t;
        if (!ParseTuple(typeStr, t)) continue;

        Entry e;
        e.name        = name;
        e.kind        = static_cast<uint8_t>(t[0]);
        e.domain      = static_cast<uint8_t>(t[1]);
        e.category    = static_cast<uint8_t>(t[2]);
        e.subcategory = static_cast<uint8_t>(t[3]);
        if (t.size() == 5) { e.specific = static_cast<uint8_t>(t[4]); e.hasSpecific = true; }
        m_entries.push_back(e);
    }

    // Most-specific-first, stable so ties keep file order — the same ordering
    // FDISConfigLoader::LoadHangerCalibrations applies, so both sides pick the
    // same winner when a 5-part and a 4-part entry both match.
    std::stable_sort(m_entries.begin(), m_entries.end(),
        [](const Entry& a, const Entry& b) { return a.hasSpecific && !b.hasSpecific; });

    return !m_entries.empty();
}

std::string HangerCameraTypeMap::ResolveTypeKey(uint8_t kind, uint8_t domain, uint8_t category,
                                                uint8_t subcategory, uint8_t specific) const
{
    for (const Entry& e : m_entries)
    {
        if (e.kind != kind || e.domain != domain
            || e.category != category || e.subcategory != subcategory) continue;
        if (e.hasSpecific && e.specific != specific) continue;
        return e.name;
    }
    return std::string();
}
