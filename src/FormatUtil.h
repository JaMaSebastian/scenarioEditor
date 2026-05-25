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
//   "Mach 1.2"   -> 411.6 m/s
//   "M 1.2"      -> 411.6
//   "M1.2"       -> 411.6
//   "1.2 Mach"   -> 411.6
//   "1.2M"       -> 411.6  (trailing M, no separator)
// Case-insensitive. Conversion uses 343 m/s (ISA sea-level speed of sound).
// Returns NaN if the text can't be parsed as a number.
inline double ParseSpeedMps(const CString& text)
{
    constexpr double kMachToMps = 343.0;   // ISA sea-level

    // Narrow + lowercase for parsing.
    CT2A ascii(text);
    if (!ascii.m_psz) return std::nan("");
    std::string buf(ascii.m_psz);

    // Detect a "Mach" or standalone "M" cue anywhere in the string.
    bool isMach = false;
    std::string lower; lower.reserve(buf.size());
    for (char c : buf) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower.find("mach") != std::string::npos)
    {
        isMach = true;
    }
    else
    {
        // Standalone 'm' (not part of another letter). Common forms:
        // "M 1.2", "M1.2", "1.2M", "1.2 M". Reject if 'm' appears next
        // to another letter (avoids matching unit suffixes like "mph"
        // or "mile" if they slip in).
        for (size_t i = 0; i < lower.size(); ++i)
        {
            if (lower[i] != 'm') continue;
            const bool prevIsAlpha = (i > 0 && std::isalpha(static_cast<unsigned char>(lower[i-1])));
            const bool nextIsAlpha = (i+1 < lower.size() &&
                                      std::isalpha(static_cast<unsigned char>(lower[i+1])));
            if (!prevIsAlpha && !nextIsAlpha) { isMach = true; break; }
        }
    }

    // Strip all alphabetic chars and re-parse the residue as a number.
    std::string numeric; numeric.reserve(buf.size());
    for (char c : buf)
        if (!std::isalpha(static_cast<unsigned char>(c))) numeric.push_back(c);

    char* end = nullptr;
    const double n = std::strtod(numeric.c_str(), &end);
    if (end == numeric.c_str()) return std::nan("");

    return isMach ? (n * kMachToMps) : n;
}
