//=============================================================================
//  FormatUtil.h
//-----------------------------------------------------------------------------
//  Header-only display/parse helpers for numeric edit fields: FormatDoubleTrim
//  renders a double at fixed precision with trailing zeros stripped, and
//  ParseSpeedMps parses a free-form speed string (raw m/s, Mach, mph, km/h)
//  into metres per second.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-25
//=============================================================================
#pragma once

#include "pch.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Format a double for display in an edit control: full precision but
// with trailing zeros after the decimal stripped (always keeping at
// least one digit after the dot). Examples:
//   0.000000  ->  "0.0"
//   1.000000  ->  "1.0"
//   1.008000  ->  "1.008"
//   37.774900 ->  "37.7749"
//   -122.4194 ->  "-122.4194"
//   1234.5    ->  "1234.5"
// Very large / very small magnitudes still use fixed notation (no
// scientific) so the user sees readable numbers in the edit fields.
inline CString FormatDoubleTrim(double v, int maxFractionalDigits = 6)
{
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "%.*f", maxFractionalDigits, v);

    // Locate the decimal point (always present since maxFractional > 0).
    char* dot = std::strchr(tmp, '.');
    if (dot)
    {
        // Strip trailing zeros, but stop before consuming the final digit
        // immediately after the dot — we always keep at least one decimal.
        char* end = tmp + std::strlen(tmp) - 1;
        while (end > dot + 1 && *end == '0')
            *end-- = '\0';
    }
    return CString(tmp);
}

// Parse a speed input string. Accepts:
//   "260"        -> 260 m/s
//   "Mach 1.2"   -> 411.6 m/s     (343 * 1.2, ISA sea level)
//   "M 1.2"      -> 411.6         (lone "M" treated as Mach)
//   "600 mph"    -> 268.224       (0.44704 m/s per mph)
//   "1000 km/h"  -> 277.778       (1 / 3.6 m/s per km/h)
//   "1000 kph"   -> 277.778       (same; "kph" / "kmh" / "km/h" all accepted)
// Case-insensitive. Unit detection order: mph -> km/h -> mach -> bare M.
// (mph and km/h are checked first so their embedded 'm' / 'k' don't trip
// the Mach shorthand.)
// Returns NaN if the text can't be parsed as a number.
inline double ParseSpeedMps(const CString& text)
{
    constexpr double kMachToMps = 343.0;        // ISA sea-level speed of sound
    constexpr double kMphToMps  = 0.44704;      // statute mph -> m/s
    constexpr double kKphToMps  = 1.0 / 3.6;    // km/h -> m/s (~0.27778)

    // Narrow + lowercase for parsing.
    CT2A ascii(text);
    if (!ascii.m_psz) return std::nan("");
    std::string buf(ascii.m_psz);

    std::string lower; lower.reserve(buf.size());
    for (char c : buf) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // Pick the multiplier from the unit cue. Order matters: check the
    // multi-letter unit suffixes BEFORE the bare-M Mach shorthand, since
    // both "mph" and "kmh" contain an 'm' that the shorthand check would
    // otherwise interpret as Mach.
    double multiplier = 1.0;
    if (lower.find("mph") != std::string::npos)
    {
        multiplier = kMphToMps;
    }
    else if (lower.find("km/h") != std::string::npos ||
             lower.find("kmh")  != std::string::npos ||
             lower.find("kph")  != std::string::npos)
    {
        multiplier = kKphToMps;
    }
    else if (lower.find("mach") != std::string::npos)
    {
        multiplier = kMachToMps;
    }
    else
    {
        // Standalone 'm' (not part of another letter). Common forms:
        // "M 1.2", "M1.2", "1.2M", "1.2 M".
        for (size_t i = 0; i < lower.size(); ++i)
        {
            if (lower[i] != 'm') continue;
            const bool prevIsAlpha = (i > 0 && std::isalpha(static_cast<unsigned char>(lower[i-1])));
            const bool nextIsAlpha = (i+1 < lower.size() &&
                                      std::isalpha(static_cast<unsigned char>(lower[i+1])));
            if (!prevIsAlpha && !nextIsAlpha) { multiplier = kMachToMps; break; }
        }
    }

    // Strip alphabetic chars and re-parse the residue as a number.
    // (The '/' in "km/h" is non-alpha and survives the strip; strtod
    // happily stops at it after consuming the leading number.)
    std::string numeric; numeric.reserve(buf.size());
    for (char c : buf)
        if (!std::isalpha(static_cast<unsigned char>(c))) numeric.push_back(c);

    char* end = nullptr;
    const double n = std::strtod(numeric.c_str(), &end);
    if (end == numeric.c_str()) return std::nan("");

    return n * multiplier;
}
