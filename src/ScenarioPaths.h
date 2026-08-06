//=============================================================================
//  ScenarioPaths.h
//-----------------------------------------------------------------------------
//  Small shared path helpers for the ScenarioEditor. Include AFTER pch.h (relies
//  on CString/_T already being available from the MFC precompiled header).
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-16
//=============================================================================
#pragma once

//
// CameraPathFor — derive the camera side-file path for a scenario .ini:
//   "<base>.ini" -> "<base>-camera.ini"  (an extensionless path just gets
//   "-camera.ini" appended). The camera schedule lives next to the scenario and
//   is written by ScenarioCameraIO::Save on every scenario save.
//
inline CString CameraPathFor(const CString& scenarioIni)
{
    const int dot = scenarioIni.ReverseFind(_T('.'));
    const int bslash = scenarioIni.ReverseFind(_T('\\'));
    const int fslash = scenarioIni.ReverseFind(_T('/'));
    const int slash = (bslash > fslash) ? bslash : fslash;
    if (dot > slash)   // has an extension after the last separator
        return scenarioIni.Left(dot) + _T("-camera.ini");
    return scenarioIni + _T("-camera.ini");
}
