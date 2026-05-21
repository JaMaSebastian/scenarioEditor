#include "pch.h"
#include "PreviewPage.h"

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_BTN_PREVIEW_START,       _T("Start the preview animation."), false },
        { IDC_BTN_PREVIEW_PAUSE,       _T("Pause the preview animation."), false },
        { IDC_BTN_PREVIEW_RESUME,      _T("Resume the paused preview animation."), false },
        { IDC_BTN_PREVIEW_STOP,        _T("Stop the preview and reset to time zero."), false },
        { IDC_BTN_PREVIEW_ZOOM_IN,     _T("Zoom the preview canvas in."), false },
        { IDC_BTN_PREVIEW_ZOOM_OUT,    _T("Zoom the preview canvas out."), false },
        { IDC_BTN_PREVIEW_FIT,         _T("Fit the entire scenario in the preview canvas."), false },
        { IDC_CHK_PREVIEW_LABELS,      _T("Show entity labels in the preview."), false },
        { IDC_CHK_PREVIEW_TRAILS,      _T("Show motion trails behind moving entities."), false },
        { IDC_CHK_PREVIEW_PATHS,       _T("Show configured motion paths."), false },
        { IDC_CHK_PREVIEW_ORIENTATION, _T("Show heading / pitch / roll vectors on each entity."), false },
    };
}

BEGIN_MESSAGE_MAP(CPreviewPage, CHelpAwarePage)
END_MESSAGE_MAP()

void CPreviewPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

BOOL CPreviewPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();
    return TRUE;
}
