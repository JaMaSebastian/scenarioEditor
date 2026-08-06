//=============================================================================
//  CameraZoomMenu.h
//-----------------------------------------------------------------------------
//  Declares the shared per-frame camera right-click submenu (shown as "Camera").
//  Two different controls own camera boxes — the Preview canvas (pink stationary
//  vantage squares) and the camera timeline strip (every frame, both kinds) — and
//  both need the same items, so the menu is built and dispatched here instead of
//  being written twice.
//
//  It has since grown past zoom to also carry the gimbal-limits toggle; the file
//  keeps its original name rather than churning three call sites for a rename.
//
//  Only the frame whose timeline box contains the scenario clock is ever live, so
//  these settings are per-frame: DISBrowser applies them while that box owns the
//  clock and reverts on the next cut.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-28
//=============================================================================
#pragma once

#include <afxwin.h>
#include <cstddef>

struct CameraFrame;
class  CPreviewPage;

namespace CameraZoomMenu
{
    // Command IDs this menu owns. Call sites must keep their own local menu
    // enums outside [kCmdFirst, kCmdLast] so the TrackPopupMenu result stays
    // unambiguous.
    constexpr UINT kCmdFirst = 6000;
    constexpr UINT kCmdLast  = 6099;

    // Populate `sub` (an already-created popup) with the zoom items for `f`.
    // Checkmarks mirror the frame's current state; items that don't apply are
    // grayed rather than hidden, so the menu shape stays stable.
    void Build(CMenu& sub, const CameraFrame& f);

    // Dispatch a TrackPopupMenu result. Returns false — having done nothing — when
    // `cmd` isn't one of ours, so callers can chain it ahead of their own handling.
    // `parent` owns any prompt the command needs.
    bool Handle(UINT cmd, CPreviewPage& page, size_t frameIdx,
                const CameraFrame& f, CWnd* parent);
}
