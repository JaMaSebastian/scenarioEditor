//=============================================================================
//  camera_preset_io_test.cpp
//-----------------------------------------------------------------------------
//  Round-trips the gimbal envelope through CameraPresetIO::WriteEnvelope and
//  CameraPresetCatalog::LoadFromIni against a scratch Cameras.ini in %TEMP%.
//
//  The cases that matter are the ones a well-meaning refactor would break:
//
//    * yaw/roll are ARCS — Min > Max is legal and must survive unswapped. This is
//      the regression a "helpful" normalisation would introduce, and it silently
//      inverts a camera's travel envelope rather than failing loudly.
//    * pitch is an INTERVAL — a reversed pair IS swapped, and both ends clamp to
//      [-90,90], mirroring what DISBrowser does on load.
//    * writes are surgical — neighbouring sections, non-envelope keys, and the
//      optional DIS-only Angle/Target/Default keys all survive.
//    * clearing deletes the seven keys rather than zeroing them, so a preset that
//      never had an envelope reads back identical to a pre-feature file.
//    * a section that does not exist is refused, not appended.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-05
//=============================================================================
#include "CameraPresetIO.h"
#include "CameraPresetCatalog.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void Expect(bool cond, const char* what)
    {
        if (!cond) { std::printf("  FAIL %s\n", what); ++failures; }
    }

    void ExpectNear(double got, double want, const char* what)
    {
        if (std::fabs(got - want) > 1e-6)
        {
            std::printf("  FAIL %s (got %.6f, want %.6f)\n", what, got, want);
            ++failures;
        }
    }

    // A miniature Cameras.ini shaped like the real one: BOM, header comments, a
    // section carrying the optional DIS-only keys, and a trailing comment.
    const char* const kFixture =
        "\xEF\xBB\xBF; Saved camera views, keyed by entity TYPE.\r\n"
        "; LimitsEnabled/Min*/Max* are the optional body-relative gimbal envelope.\r\n"
        "\r\n"
        "[CameraViews.Alpha.front]\r\n"
        "X=1.500000\r\n"
        "Y=2.500000\r\n"
        "Z=3.500000\r\n"
        "Pitch=10.000000\r\n"
        "Yaw=20.000000\r\n"
        "Roll=30.000000\r\n"
        "FOV=75.000000\r\n"
        "FocusCenter=0\r\n"
        "\r\n"
        "[CameraViews.Beta.back]\r\n"
        "X=-9.000000\r\n"
        "Y=0.000000\r\n"
        "Z=4.000000\r\n"
        "Pitch=-5.000000\r\n"
        "Yaw=0.000000\r\n"
        "Roll=0.000000\r\n"
        "FOV=90.000000\r\n"
        "FocusCenter=1\r\n"
        "Angle=3\r\n"
        "Target=1.1.5\r\n"
        "Default=1\r\n"
        "\r\n"
        "[CameraViews.Gamma.left]\r\n"
        "X=0.000000\r\n"
        "Y=7.000000\r\n"
        "Z=1.000000\r\n"
        "Pitch=0.000000\r\n"
        "Yaw=0.000000\r\n"
        "Roll=0.000000\r\n"
        "FOV=60.000000\r\n"
        "FocusCenter=0\r\n"
        "; trailing comment that must survive\r\n";

    std::wstring MakeFixture()
    {
        wchar_t tmp[MAX_PATH] = { 0 };
        ::GetTempPathW(_countof(tmp), tmp);
        const std::wstring path = std::wstring(tmp) + L"camera_preset_io_test.ini";

        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD wrote = 0;
        ::WriteFile(h, kFixture, static_cast<DWORD>(std::strlen(kFixture)), &wrote, nullptr);
        ::CloseHandle(h);
        return path;
    }

    std::string ReadAll(const std::wstring& path)
    {
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        std::string out;
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
            out.append(buf, got);
        ::CloseHandle(h);
        return out;
    }

    bool Has(const std::string& hay, const char* needle)
    {
        return hay.find(needle) != std::string::npos;
    }

    const CameraPreset* Load(CameraPresetCatalog& cat, const std::wstring& path,
                             const char* type, const char* angle)
    {
        cat.LoadFromIni(path);
        return cat.Find(type, angle);
    }
}

int main()
{
    const std::wstring path = MakeFixture();
    if (path.empty())
    {
        std::printf("FATAL: could not create the fixture in %%TEMP%%\n");
        return 1;
    }
    std::printf("camera_preset_io_test: %S\n", path.c_str());

    CameraPresetCatalog cat;
    std::wstring err;

    // ---- Case 0: the fixture parses, and carries no envelope to begin with ----
    {
        const CameraPreset* p = Load(cat, path, "Alpha", "front");
        Expect(p != nullptr, "fixture: Alpha/front parses");
        if (p)
        {
            Expect(!p->hasLimits,        "fixture: Alpha/front has no envelope");
            Expect(!p->LimitsAvailable(),"fixture: Alpha/front envelope unavailable");
            // The permissive defaults are what an un-authored preset reports.
            ExpectNear(p->minPitch, -90.0,  "fixture: default minPitch");
            ExpectNear(p->maxYaw,   180.0,  "fixture: default maxYaw");
        }
        Expect(cat.Count() == 3, "fixture: three presets");
    }

    // ---- Case 1: write an envelope; every field round-trips ----
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined  = true;
        env.enabled  = true;
        env.minPitch = -30.0; env.maxPitch =  20.0;
        env.minYaw   = -45.0; env.maxYaw   =  45.0;
        env.minRoll  = -15.0; env.maxRoll  =  15.0;

        Expect(CameraPresetIO::WriteEnvelope(path, "Alpha", "front", env, err),
               "write: Alpha/front accepted");

        const CameraPreset* p = Load(cat, path, "Alpha", "front");
        Expect(p != nullptr, "write: Alpha/front still parses");
        if (p)
        {
            Expect(p->hasLimits,         "write: hasLimits set");
            Expect(p->limitsEnabled,     "write: limitsEnabled set");
            Expect(p->LimitsAvailable(), "write: envelope now available");
            ExpectNear(p->minPitch, -30.0, "write: minPitch");
            ExpectNear(p->maxPitch,  20.0, "write: maxPitch");
            ExpectNear(p->minYaw,   -45.0, "write: minYaw");
            ExpectNear(p->maxYaw,    45.0, "write: maxYaw");
            ExpectNear(p->minRoll,  -15.0, "write: minRoll");
            ExpectNear(p->maxRoll,   15.0, "write: maxRoll");

            // Surgical: the section's own non-envelope keys are untouched.
            ExpectNear(p->x,    1.5, "write: X survived");
            ExpectNear(p->fov, 75.0, "write: FOV survived");
        }
    }

    // ---- Case 2: neighbouring sections and their optional DIS keys survive ----
    {
        const CameraPreset* b = Load(cat, path, "Beta", "back");
        Expect(b != nullptr, "neighbour: Beta/back still parses");
        if (b)
        {
            ExpectNear(b->x,   -9.0, "neighbour: Beta X untouched");
            ExpectNear(b->fov, 90.0, "neighbour: Beta FOV untouched");
            Expect(!b->hasLimits, "neighbour: Beta gained no envelope");
        }
        Expect(cat.Count() == 3, "neighbour: still three presets (nothing appended)");

        const std::string text = ReadAll(path);
        Expect(Has(text, "Angle=3"),     "neighbour: Angle= survived");
        Expect(Has(text, "Target=1.1.5"),"neighbour: Target= survived");
        Expect(Has(text, "Default=1"),   "neighbour: Default= survived");
        Expect(Has(text, "; trailing comment that must survive"),
               "neighbour: trailing comment survived");
        Expect(Has(text, "[CameraViews.Gamma.left]"), "neighbour: Gamma section survived");
    }

    // ---- Case 3: a WRAPPING yaw arc round-trips unswapped ----
    //   Min > Max is a legal arc through +/-180. Swapping it would turn this
    //   120-degree cone into a 240-degree one pointing the other way.
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined = true;
        env.enabled = true;
        env.minYaw  = 115.0;  env.maxYaw  = -125.0;
        env.minRoll = 170.0;  env.maxRoll = -170.0;

        Expect(CameraPresetIO::WriteEnvelope(path, "Gamma", "left", env, err),
               "wrap: Gamma/left accepted");

        const CameraPreset* p = Load(cat, path, "Gamma", "left");
        Expect(p != nullptr, "wrap: Gamma/left parses");
        if (p)
        {
            ExpectNear(p->minYaw,   115.0, "wrap: minYaw NOT swapped");
            ExpectNear(p->maxYaw,  -125.0, "wrap: maxYaw NOT swapped");
            ExpectNear(p->minRoll,  170.0, "wrap: minRoll NOT swapped");
            ExpectNear(p->maxRoll, -170.0, "wrap: maxRoll NOT swapped");
        }
    }

    // ---- Case 4: reversed pitch IS swapped, and out-of-range pitch clamps ----
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined  = true;
        env.enabled  = true;
        env.minPitch = 40.0; env.maxPitch = 10.0;

        Expect(CameraPresetIO::WriteEnvelope(path, "Gamma", "left", env, err),
               "pitch: reversed pair accepted");
        const CameraPreset* p = Load(cat, path, "Gamma", "left");
        if (p)
        {
            ExpectNear(p->minPitch, 10.0, "pitch: reversed pair swapped low");
            ExpectNear(p->maxPitch, 40.0, "pitch: reversed pair swapped high");
        }

        env.minPitch = -140.0; env.maxPitch = 175.0;
        Expect(CameraPresetIO::WriteEnvelope(path, "Gamma", "left", env, err),
               "pitch: out-of-range pair accepted");
        p = Load(cat, path, "Gamma", "left");
        if (p)
        {
            ExpectNear(p->minPitch, -90.0, "pitch: clamped to -90");
            ExpectNear(p->maxPitch,  90.0, "pitch: clamped to +90");
        }
    }

    // ---- Case 5: LimitsEnabled=0 is "defined but not enforced" ----
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined = true;
        env.enabled = false;

        Expect(CameraPresetIO::WriteEnvelope(path, "Alpha", "front", env, err),
               "disabled: accepted");
        const CameraPreset* p = Load(cat, path, "Alpha", "front");
        if (p)
        {
            Expect(p->hasLimits,          "disabled: still defined");
            Expect(!p->limitsEnabled,     "disabled: not enforced");
            Expect(!p->LimitsAvailable(), "disabled: unavailable to the UI");
        }
    }

    // ---- Case 6: clearing DELETES all seven keys ----
    {
        CameraPresetIO::GimbalEnvelope env;   // defined = false
        Expect(CameraPresetIO::WriteEnvelope(path, "Alpha", "front", env, err),
               "clear: accepted");

        const CameraPreset* p = Load(cat, path, "Alpha", "front");
        Expect(p != nullptr, "clear: Alpha/front still parses");
        if (p)
        {
            Expect(!p->hasLimits, "clear: envelope gone, not zeroed");
            // Back to the pre-feature reading, field for field.
            ExpectNear(p->minPitch, -90.0, "clear: minPitch back to default");
            ExpectNear(p->maxPitch,  90.0, "clear: maxPitch back to default");
            ExpectNear(p->minYaw,  -180.0, "clear: minYaw back to default");
            ExpectNear(p->maxYaw,   180.0, "clear: maxYaw back to default");
            ExpectNear(p->x,          1.5, "clear: X still intact");
            ExpectNear(p->fov,       75.0, "clear: FOV still intact");
        }

        // The keys are really gone from Alpha's section, not merely re-parsed away.
        // (Gamma still carries an envelope, so search Alpha's slice of the file.)
        const std::string text = ReadAll(path);
        const size_t a = text.find("[CameraViews.Alpha.front]");
        const size_t b = text.find("[CameraViews.Beta.back]");
        Expect(a != std::string::npos && b != std::string::npos && a < b,
               "clear: section order intact");
        if (a != std::string::npos && b != std::string::npos && a < b)
        {
            const std::string alpha = text.substr(a, b - a);
            Expect(!Has(alpha, "LimitsEnabled"), "clear: LimitsEnabled key deleted");
            Expect(!Has(alpha, "MinPitch"),      "clear: MinPitch key deleted");
            Expect(!Has(alpha, "MaxRoll"),       "clear: MaxRoll key deleted");
        }
    }

    // ---- Case 7: an unknown section is REFUSED, not appended ----
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined = true;

        const size_t before = cat.Count();
        Expect(!CameraPresetIO::WriteEnvelope(path, "NoSuchType", "front", env, err),
               "unknown: refused");
        Expect(!err.empty(), "unknown: reports why");

        cat.LoadFromIni(path);
        Expect(cat.Count() == before, "unknown: nothing appended to the file");
        Expect(cat.Find("NoSuchType", "front") == nullptr, "unknown: no phantom preset");
    }

    // ---- Case 8: a missing file is an error, not a silent create ----
    {
        CameraPresetIO::GimbalEnvelope env;
        env.defined = true;
        const std::wstring missing = path + L".does-not-exist";
        Expect(!CameraPresetIO::WriteEnvelope(missing, "Alpha", "front", env, err),
               "missing file: refused");
        Expect(::GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES,
               "missing file: not created");
    }

    ::DeleteFileW(path.c_str());

    if (failures == 0) std::printf("camera_preset_io_test: ALL PASS\n");
    else               std::printf("camera_preset_io_test: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
