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

    void SortById(std::vector<CatalogEntry>& v)
    {
        std::sort(v.begin(), v.end(),
                  [](const CatalogEntry& a, const CatalogEntry& b) { return a.id < b.id; });
    }
}

const std::vector<CatalogEntry>& EntityTypeCatalog::Domains(uint8_t kindId) const
{
    auto it = m_domainsByKind.find(kindId);
    return (it == m_domainsByKind.end()) ? m_empty : it->second;
}

const std::vector<CatalogEntry>& EntityTypeCatalog::Categories(uint8_t kindId,
                                                                uint8_t domainId) const
{
    auto it = m_categoriesByKindDomain.find({ kindId, domainId });
    return (it == m_categoriesByKindDomain.end()) ? m_empty : it->second;
}

const std::vector<CatalogEntry>& EntityTypeCatalog::Subcategories(uint8_t kindId,
                                                                  uint8_t domainId,
                                                                  uint8_t categoryId) const
{
    auto it = m_subcategoriesByKindDomainCategory.find({ kindId, domainId, categoryId });
    return (it == m_subcategoriesByKindDomainCategory.end()) ? m_empty : it->second;
}

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
        categoryCount = 0, subcategoryCount = 0, skipped = 0;

    for (DWORD i = 0; i < nameLen; )
    {
        const wchar_t* sec = namesBuf.data() + i;
        const size_t len = std::wcslen(sec);
        i += static_cast<DWORD>(len + 1);
        if (len == 0) continue;

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
            m_categoriesByKindDomain[{ static_cast<uint8_t>(numbers[0]),
                                       static_cast<uint8_t>(numbers[1]) }]
                .push_back({ static_cast<uint16_t>(numbers[2]), narrowName });
            ++categoryCount;
        }
        else if (ParseSection(sec, L"Subcategory", 4, numbers))
        {
            m_subcategoriesByKindDomainCategory[{ static_cast<uint8_t>(numbers[0]),
                                                  static_cast<uint8_t>(numbers[1]),
                                                  static_cast<uint8_t>(numbers[2]) }]
                .push_back({ static_cast<uint16_t>(numbers[3]), narrowName });
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
              "%d categories, %d subcategories (skipped %d)",
              kindCount, domainCount, countryCount,
              categoryCount, subcategoryCount, skipped);
    LOG(szError);
    return Loaded();
}
