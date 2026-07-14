//=============================================================================
//  framework.h
//-----------------------------------------------------------------------------
//  Central MFC framework include header. Pulls in the core MFC libraries
//  (afxwin, afxext, OLE/ODBC, common controls, control bars, dialogex) plus
//  the Direct2D/DirectWrite headers used by the Preview tab's animated canvas,
//  and sets the Windows lean-and-mean / NOMINMAX build switches.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// Suppress windows.h's max/min macros so std::max / std::min compile cleanly
// in TUs that include <algorithm> alongside Windows headers (PreviewPage,
// PreviewCanvas, etc.).
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "targetver.h"

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#include <afxwin.h>
#include <afxext.h>
#include <afxdisp.h>

#ifndef _AFX_NO_OLE_SUPPORT
#include <afxdtctl.h>
#endif
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>
#endif

#include <afxcontrolbars.h>
#include <afxdialogex.h>

// Direct2D + DirectWrite for the Preview tab's animated canvas. Libs are
// linked in the .vcxproj; headers go here so any TU can pull D2D types.
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
