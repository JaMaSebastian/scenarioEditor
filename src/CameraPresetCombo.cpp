//=============================================================================
//  CameraPresetCombo.cpp
//-----------------------------------------------------------------------------
//  Implements CameraPresetCombo: the one place that decides what goes in a
//  "Camera" preset dropdown and in what order. Previously duplicated between
//  CPreviewPage::RefreshCameraPresetCombo and CEntityCameraDialog::OnInitDialog,
//  which had to be kept in lockstep by hand.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-04
//=============================================================================
#include "pch.h"
#include "CameraPresetCombo.h"
#include "ScenarioEditor.h"        // theApp + CameraPresets() + HangerTypes()
#include "CameraPresetCatalog.h"
#include "Scenario.h"

//
// TypeKeyForEntityId — see header.
//
std::string CameraPresetCombo::TypeKeyForEntityId(const Scenario& s, uint16_t entityId)
{
    if (entityId == 0) return std::string();
    for (const Entity& e : s.entities)
    {
        if (e.entityId != entityId) continue;
        return theApp.HangerTypes().ResolveTypeKey(
            e.kind, e.domain, e.category, e.subcategory, e.specific);
    }
    return std::string();
}

//
// Populate — see header.
//
int CameraPresetCombo::Populate(CComboBox& combo, const std::string& typeKey)
{
    combo.ResetContent();
    const auto& presets = theApp.CameraPresets().Presets();

    // The entity's own type first, so the right answer is the default answer.
    int firstMatch = -1;
    if (!typeKey.empty())
    {
        for (size_t i = 0; i < presets.size(); ++i)
        {
            if (presets[i].type != typeKey) continue;
            const int ix = combo.AddString(CString(CA2W(presets[i].DisplayLabel().c_str())));
            combo.SetItemData(ix, static_cast<DWORD_PTR>(i));
            if (firstMatch < 0) firstMatch = ix;
        }
    }

    // Nothing for this entity: say so rather than silently offering another
    // airframe's views as if they belonged. This is the state that produced the
    // LUCAS-on-EGAT shots — EGOT had no presets yet, so there was no right answer
    // to pick and the nearest UAS got used instead. Name the missing prerequisite,
    // because neither fix (author them in the hanger / set the DISBrowser folder)
    // is guessable from an empty list.
    if (firstMatch < 0)
    {
        CString notice;
        if (typeKey.empty())
            notice = theApp.HangerTypes().Loaded()
                ? _T("(this entity has no hanger calibration — no camera views)")
                : _T("(set the DISBrowser folder on the Run tab to filter by entity)");
        else
            notice.Format(_T("(no camera views authored for '%s')"),
                          (LPCTSTR)CString(CA2W(typeKey.c_str())));
        const int ix = combo.AddString(notice);
        combo.SetItemData(ix, kRowNotSelectable);
    }

    // Other types stay reachable below a separator rather than being removed:
    // mounting a deliberate stand-in is legitimate, and hiding it would only push
    // people back to hand-editing the ini.
    if (!presets.empty())
    {
        const int sep = combo.AddString(_T("--------- other types ---------"));
        combo.SetItemData(sep, kRowNotSelectable);
        for (size_t i = 0; i < presets.size(); ++i)
        {
            if (!typeKey.empty() && presets[i].type == typeKey) continue;   // already listed
            const int ix = combo.AddString(CString(CA2W(presets[i].DisplayLabel().c_str())));
            combo.SetItemData(ix, static_cast<DWORD_PTR>(i));
        }
    }

    return firstMatch;
}

//
// PopulateNoSource — see header. A Stationary camera is a point in space, not a
//   mount, so it carries no presetType/presetAngle at all. Saying that in the
//   list beats an empty disabled combo, which reads as a bug.
//
void CameraPresetCombo::PopulateNoSource(CComboBox& combo)
{
    combo.ResetContent();
    const int ix = combo.AddString(_T("(stationary camera — no camera type)"));
    combo.SetItemData(ix, kRowNotSelectable);
    combo.SetCurSel(0);
}
