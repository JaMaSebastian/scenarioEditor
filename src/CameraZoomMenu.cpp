//=============================================================================
//  CameraZoomMenu.cpp
//-----------------------------------------------------------------------------
//  Implements the shared "Zoom" submenu: two checkable toggles plus two numeric
//  prompts, dispatched onto CPreviewPage's zoom mutators. The prompts reuse the
//  IDD_PROMPT_NAME template (same trick as CatalogEditorDialog and the Preview
//  page's own prompt) with range validation layered on top.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-28
//=============================================================================
#include "pch.h"
#include "CameraZoomMenu.h"
#include "Resource.h"
#include "Scenario.h"
#include "PreviewPage.h"
#include "ScenarioEditor.h"        // theApp.CameraPresets()
#include "CameraPresetCatalog.h"

namespace
{
    enum : UINT
    {
        kToggleZoom = CameraZoomMenu::kCmdFirst,   // 6000
        kToggleDyn,                                // 6001
        kSetFov,                                   // 6002
        kSetFill,                                  // 6003
        kToggleGimbal,                             // 6004
    };

    //
    // CNumberPromptDialog — one-field modal over IDD_PROMPT_NAME that accepts a
    //   decimal in [lo, hi]. Out-of-range input keeps the dialog open, matching
    //   how every other numeric entry in the app rejects bad values.
    //
    class CNumberPromptDialog : public CDialogEx
    {
    public:
        enum { IDD = IDD_PROMPT_NAME };
        CNumberPromptDialog(const CString& caption, const CString& prompt,
                            double initial, double lo, double hi, CWnd* parent)
            : CDialogEx(IDD, parent), m_caption(caption), m_prompt(prompt),
              m_value(initial), m_lo(lo), m_hi(hi)
        {}
        double Value() const { return m_value; }

    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            SetWindowText(m_caption);
            SetDlgItemText(IDC_LBL_PROMPT, m_prompt);
            CString s;
            s.Format(_T("%g"), m_value);
            SetDlgItemText(IDC_EDIT_PROMPT, s);
            if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_PROMPT))
            {
                e->SetSel(0, -1);
                e->SetFocus();
                return FALSE;   // we set focus ourselves
            }
            return TRUE;
        }
        void OnOK() override
        {
            CString txt;
            GetDlgItemText(IDC_EDIT_PROMPT, txt);
            txt.Trim();
            const double v = _tstof(txt);
            if (txt.IsEmpty() || v < m_lo || v > m_hi)
            {
                CString msg;
                msg.Format(_T("Enter a value between %g and %g."), m_lo, m_hi);
                AfxMessageBox(msg);
                return;   // keep the dialog open
            }
            m_value = v;
            CDialogEx::OnOK();
        }

        CString m_caption, m_prompt;
        double  m_value, m_lo, m_hi;
    };
}

//
// Build — see header.
//
void CameraZoomMenu::Build(CMenu& sub, const CameraFrame& f)
{
    const UINT zoomFlags = MF_STRING | (f.zoomEnabled ? MF_CHECKED : MF_UNCHECKED);
    sub.AppendMenu(zoomFlags, kToggleZoom, _T("Zoom enabled"));

    // Dynamic needs zoom on AND a catalogued target size to fit to. Say which is
    // missing in the item text — a silently grayed toggle reads as a bug, and the
    // fix (dimensions in EntityTypeCatalog.ini) isn't guessable otherwise.
    const bool hasSize = f.HasTargetSize();
    UINT dynFlags = MF_STRING | (f.dynamicZoom ? MF_CHECKED : MF_UNCHECKED);
    if (!f.zoomEnabled || !hasSize) dynFlags |= MF_GRAYED;
    sub.AppendMenu(dynFlags, kToggleDyn,
                   hasSize ? _T("Dynamic zoom (fit target)")
                           : _T("Dynamic zoom (target has no size in the entity catalog)"));

    sub.AppendMenu(MF_SEPARATOR, 0, static_cast<LPCTSTR>(nullptr));

    CString label;

    // The static FOV and the dynamic fill are mutually exclusive — showing both
    // but graying the one that isn't driving keeps it clear which number is live.
    label.Format(_T("Set FOV...\t%g\xB0"), f.zoomFovDeg);
    sub.AppendMenu(MF_STRING | ((f.zoomEnabled && !f.dynamicZoom) ? 0 : MF_GRAYED),
                   kSetFov, label);

    label.Format(_T("Set Fill %%...\t%g%%"), f.zoomFillPct);
    sub.AppendMenu(MF_STRING | ((f.zoomEnabled && f.dynamicZoom) ? 0 : MF_GRAYED),
                   kSetFill, label);

    sub.AppendMenu(MF_SEPARATOR, 0, static_cast<LPCTSTR>(nullptr));

    // Gimbal limits need a mounted preset that actually defines and enables an envelope,
    // which only Entity frames have. Same convention as Dynamic zoom above: name the
    // missing prerequisite in the item text, because the fix (authoring the envelope in
    // DISBrowser's hanger) isn't guessable from a grayed toggle.
    const CameraPreset* preset = (f.kind == CameraKind::Entity)
        ? theApp.CameraPresets().Find(f.presetType, f.presetAngle)
        : nullptr;
    const bool limitsAvail = preset && preset->LimitsAvailable();

    UINT gimFlags = MF_STRING | (f.limitsEnabled ? MF_CHECKED : MF_UNCHECKED);
    if (!limitsAvail) gimFlags |= MF_GRAYED;
    sub.AppendMenu(gimFlags, kToggleGimbal,
                   limitsAvail ? _T("Gimbal limits")
                               : _T("Gimbal limits (none defined for this camera)"));
}

//
// Handle — see header.
//
bool CameraZoomMenu::Handle(UINT cmd, CPreviewPage& page, size_t frameIdx,
                            const CameraFrame& f, CWnd* parent)
{
    switch (cmd)
    {
    case kToggleZoom:
        page.SetCameraZoomEnabled(frameIdx, !f.zoomEnabled);
        return true;

    case kToggleDyn:
        page.SetCameraDynamicZoom(frameIdx, !f.dynamicZoom);
        return true;

    case kToggleGimbal:
        page.SetCameraGimbalLimits(frameIdx, !f.limitsEnabled);
        return true;

    case kSetFov:
    {
        CNumberPromptDialog dlg(_T("Camera Zoom"), _T("Field of view (degrees):"),
                                f.zoomFovDeg, kZoomFovMinDeg, kZoomFovMaxDeg, parent);
        if (dlg.DoModal() == IDOK) page.SetCameraZoomFov(frameIdx, dlg.Value());
        return true;
    }

    case kSetFill:
    {
        CNumberPromptDialog dlg(_T("Camera Zoom"),
                                _T("Percent of the frame the target should fill:"),
                                f.zoomFillPct, kZoomFillMinPct, kZoomFillMaxPct, parent);
        if (dlg.DoModal() == IDOK) page.SetCameraZoomFill(frameIdx, dlg.Value());
        return true;
    }

    default:
        return false;
    }
}
