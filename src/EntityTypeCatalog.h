//=============================================================================
//  EntityTypeCatalog.h
//-----------------------------------------------------------------------------
//  Declares the in-memory model of EntityTypeCatalog.ini — the SISO-REF-010
//  Kind / Domain / Country / Category / Subcategory hierarchy plus per-airframe
//  physical envelopes. Backs the entity-type dropdowns, the Catalog Editor,
//  and the Validator's envelope checks.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// In-memory model of EntityTypeCatalog.ini (§7.5). Read-only after load;
// supplies the contents of the Kind / Domain / Country / Category /
// Subcategory dropdowns on the Asset / Entity Editor tab.

//-----------------------------------------------------------------------------
// CatalogEntry — one node in the catalog hierarchy (Kind/Domain/Country/
//                Category/Subcategory).
//   Carries a numeric id + display name for every node type; the attribute
//   list/values are only meaningful for Category (schema) and Subcategory
//   (values) entries.
//-----------------------------------------------------------------------------
struct CatalogEntry
{
    uint16_t    id   = 0;        // SISO-REF-010 numeric ID (uint16 covers Country)
    std::string name;            // Display name from the INI

    // ---- Category schema (Category entries only; ignored elsewhere) ----
    // Ordered list of attribute names that all Subcategories under this
    // Category carry. Modifying this list propagates to every sibling
    // subcategory via the Catalog Editor.
    std::vector<std::string> attributeNames;

    // ---- Subcategory values (Subcategory entries only; ignored elsewhere) ----
    // Keyed by attribute name. Missing key = empty / unset value. The
    // Validator's envelope checks pull well-known keys (MaxSpeedMps,
    // MaxG, etc.) out of this map at runtime.
    std::map<std::string, std::string> attributeValues;
};

// Physical-model envelope for a specific airframe (Kind/Domain/Category/
// Subcategory tuple). All fields are optional — a zero value means the
// catalog didn't supply data for that envelope, and the Validator should
// skip the corresponding check.
struct AirframeProfile
{
    bool        valid = false;       // false when no [Airframe.k.d.c.sc] section catalogued
    std::string name;

    double lengthM           = 0.0;
    double wingspanM         = 0.0;
    double heightM           = 0.0;

    double maxSpeedMps       = 0.0;
    double cruiseSpeedMps    = 0.0;
    double stallSpeedMps     = 0.0;
    double neverExceedMps    = 0.0;
    double maxTaxiMps        = 0.0;

    double serviceCeilingM   = 0.0;
    double maxAltM           = 0.0;
    double minAltM           = 0.0;

    double maxClimbRateMps     = 0.0;
    double normalClimbRateMps  = 0.0;
    double maxDescentRateMps   = 0.0;
    double normalDescentRateMps = 0.0;

    double maxBankDeg        = 0.0;
    double normalBankDeg     = 0.0;
    double maxTurnRateDps    = 0.0;
    double maxPitchDeg       = 0.0;
    double maxRollRateDps    = 0.0;
    double maxG              = 0.0;
    double maxAccelMps2      = 0.0;
    double maxDecelMps2      = 0.0;
};

//-----------------------------------------------------------------------------
// EntityTypeCatalog — loads/holds/saves the entity-type hierarchy.
//   Provides const lookup by (Kind, Domain, Category, Subcategory) for the UI,
//   mutable container access for the Catalog Editor, and INI load/save. Read-
//   only after LoadFromIni() unless edited via the Mutable* accessors.
//-----------------------------------------------------------------------------
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

    // Physical-model profile for a specific (Kind, Domain, Category,
    // Subcategory) tuple. Returns a profile with valid=false when the
    // catalog has no [Airframe.k.d.c.sc] section for that tuple — callers
    // should skip envelope checks in that case.
    AirframeProfile Profile(uint8_t kindId, uint8_t domainId,
                            uint8_t categoryId, uint8_t subcategoryId) const;

    // True if the catalog is non-empty (at least one Kind entry).
    bool Loaded() const { return !m_kinds.empty(); }

    // Load EntityTypeCatalog.ini from an absolute path. Missing file
    // returns false and logs via LOG() per §7.5; malformed entries are
    // skipped with a per-line LOG warning. Existing contents are wiped
    // before loading.
    bool LoadFromIni(const std::wstring& absolutePath);

    // Persist the in-memory catalog to disk. Always writes the *new*
    // format ([Category.*] Attribute.N keys + [Subcategory.*] key=value
    // pairs); deprecated [Airframe.*] sections are no longer emitted.
    // Returns true on success.
    bool SaveToIni(const std::wstring& absolutePath) const;

    // -------- Mutable access (for Catalog Editor) --------
    // The Catalog Editor edits a deep copy of the catalog and replaces
    // the live instance on Save. Exposing the underlying containers
    // mutably here is cheap and keeps the dialog code self-contained.
    std::vector<CatalogEntry>& MutableKinds()                     { return m_kinds; }
    std::map<uint8_t, std::vector<CatalogEntry>>& MutableDomains() { return m_domainsByKind; }
    std::vector<CatalogEntry>& MutableCountries()                 { return m_countries; }
    std::map<std::pair<uint8_t, uint8_t>, std::vector<CatalogEntry>>&
            MutableCategories()                                   { return m_categoriesByKindDomain; }
    std::map<std::tuple<uint8_t, uint8_t, uint8_t>, std::vector<CatalogEntry>>&
            MutableSubcategories()                                { return m_subcategoriesByKindDomainCategory; }

    // Reset all in-memory state. Used by the editor before reloading.
    void Clear();

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
