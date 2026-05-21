#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// In-memory model of EntityTypeCatalog.ini (§7.5). Read-only after load;
// supplies the contents of the Kind / Domain / Country / Category /
// Subcategory dropdowns on the Asset / Entity Editor tab.

struct CatalogEntry
{
    uint16_t    id   = 0;        // SISO-REF-010 numeric ID (uint16 covers Country)
    std::string name;            // Display name from the INI
};

class EntityTypeCatalog
{
public:
    // Top-level kinds.
    const std::vector<CatalogEntry>& Kinds() const { return m_kinds; }

    // Domains for a given Kind ID. Returns an empty vector if none catalogued.
    const std::vector<CatalogEntry>& Domains(uint8_t kindId) const;

    // Countries (independent of Kind/Domain).
    const std::vector<CatalogEntry>& Countries() const { return m_countries; }

    // Categories for a given (Kind, Domain). Empty if none catalogued.
    const std::vector<CatalogEntry>& Categories(uint8_t kindId, uint8_t domainId) const;

    // Subcategories for a given (Kind, Domain, Category). Empty if none catalogued.
    const std::vector<CatalogEntry>& Subcategories(uint8_t kindId, uint8_t domainId,
                                                   uint8_t categoryId) const;

    // True if the catalog is non-empty (at least one Kind entry).
    bool Loaded() const { return !m_kinds.empty(); }

    // Load EntityTypeCatalog.ini from an absolute path. Missing file
    // returns false and logs via LOG() per §7.5; malformed entries are
    // skipped with a per-line LOG warning. Existing contents are wiped
    // before loading.
    bool LoadFromIni(const std::wstring& absolutePath);

private:
    std::vector<CatalogEntry>                                 m_kinds;
    std::map<uint8_t, std::vector<CatalogEntry>>              m_domainsByKind;
    std::vector<CatalogEntry>                                 m_countries;
    std::map<std::pair<uint8_t, uint8_t>, std::vector<CatalogEntry>>
                                                              m_categoriesByKindDomain;
    std::map<std::tuple<uint8_t, uint8_t, uint8_t>, std::vector<CatalogEntry>>
                                                              m_subcategoriesByKindDomainCategory;

    // Returned by lookup methods when the requested key has no entries —
    // keeps the API const-ref-friendly without optional wrappers.
    std::vector<CatalogEntry>                                 m_empty;
};
