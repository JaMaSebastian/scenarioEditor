//=============================================================================
//  CameraPresetIO.h
//-----------------------------------------------------------------------------
//  Declares CameraPresetIO, the write half of the camera-preset story.
//  CameraPresetCatalog parses config\Cameras.ini; this writes the body-relative
//  gimbal envelope back into one [CameraViews.<Type>.<Angle>] section, in both
//  copies of the file that matter:
//
//    1. the editor's own config\Cameras.ini (theApp.CameraPresetsPath()), and
//    2. <DISBrowser project>\Config\Cameras.ini — the one DISBrowser actually
//       reads (FPaths::ProjectConfigDir(), CameraViewStore.cpp).
//
//  The two are separate files kept in step by hand. Writing only the editor's
//  copy would leave the envelope invisible at playback, which is the single most
//  confusing outcome this feature could have, so both are written together.
//
//  Scope note for callers: an envelope belongs to the PRESET, so every camera
//  shot mounting that preset — in this scenario and in every other — is affected.
//  The per-shot opt-in is a separate flag ([Camera.N] LimitsEnabled in
//  scenario.ini); DISBrowser clamps only when both are on.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-05
//=============================================================================
#pragma once

#include <string>

namespace CameraPresetIO
{
    //-------------------------------------------------------------------------
    // GimbalEnvelope — the seven envelope keys as one value.
    //   Defaults mirror DISBrowser's FCameraGimbalLimits (CameraViewStore.h):
    //   fully permissive, with roll a full circle rather than 0/0, because under
    //   arc semantics 0/0 would silently lock the horizon.
    //
    //   Pitch is a plain interval (Min <= Max, held in [-90,90]). Yaw and roll are
    //   ARCS swept Min->Max increasing: Min > Max is legal and wraps through
    //   +/-180, Min == Max locks the axis, -180/180 is unconstrained. Nothing here
    //   reorders yaw or roll.
    //-------------------------------------------------------------------------
    struct GimbalEnvelope
    {
        bool   defined = false;   // false => the seven keys are DELETED, not zeroed
        bool   enabled = true;    // LimitsEnabled: enforce by default

        double minPitch = -90.0,  maxPitch =  90.0;
        double minYaw   = -180.0, maxYaw   = 180.0;
        double minRoll  = -180.0, maxRoll  = 180.0;
    };

    //-------------------------------------------------------------------------
    // WriteEnvelope — update one preset's envelope in one Cameras.ini.
    //   Surgical: touches only the seven envelope keys inside
    //   [CameraViews.<type>.<angle>]. Every other key in that section (X/Y/Z,
    //   Pitch/Yaw/Roll, FOV, FocusCenter, and the optional DIS-only Angle/Target/
    //   Default), every other section, and every comment survive untouched.
    //
    //   env.defined == false deletes all seven keys rather than writing zeros —
    //   whole-block-or-nothing, matching CameraViewStore::SaveAll, so a preset
    //   that never had an envelope reads back identical to a pre-feature file.
    //
    //   Refuses to write a section that does not already exist: WritePrivateProfile
    //   would happily append a brand-new one at EOF, turning a typo'd type into a
    //   phantom preset. Returns false with outError set on any failure.
    //-------------------------------------------------------------------------
    bool WriteEnvelope(const std::wstring& iniPath,
                       const std::string& type, const std::string& angle,
                       const GimbalEnvelope& env, std::wstring& outError);

    // Absolute path to DISBrowser's own Cameras.ini under the given project root
    // (theApp.DisBrowserProjectDir()). Empty when that file does not exist — the
    // caller reports that as a warning, not an error, because the editor's copy
    // was still written.
    std::wstring DisBrowserCamerasIniPath(const std::wstring& disBrowserProjectDir);

    //-------------------------------------------------------------------------
    // WriteEnvelopeBothCopies — the call the dialog's caller actually makes.
    //   Takes both paths rather than reaching for theApp, the same way
    //   StartupIniWriter::Write does: it keeps this unit free of the app object so
    //   a console test can link it, and keeps the "where do the files live" policy
    //   in one place (CPreviewPage) instead of two.
    //
    //   Returns false only when the EDITOR's copy failed (outError says why).
    //   A missing or unwritable DISBrowser copy returns true with outWarning set:
    //   the authored envelope is safely on disk, it just will not reach playback
    //   until the file is synced, and that is worth a warning rather than losing
    //   the operator's work.
    //-------------------------------------------------------------------------
    bool WriteEnvelopeBothCopies(const std::wstring& editorIniPath,
                                 const std::wstring& disBrowserProjectDir,
                                 const std::string& type, const std::string& angle,
                                 const GimbalEnvelope& env,
                                 std::wstring& outWarning, std::wstring& outError);
}
