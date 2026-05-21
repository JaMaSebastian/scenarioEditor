// Round-trip test: write 100 synthetic records, read them back, byte-compare.
#include "DisrecReader.h"
#include "DisrecWriter.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    wchar_t dir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, dir);
    char path[MAX_PATH * 2];
    ::WideCharToMultiByte(CP_UTF8, 0, dir, -1, path, sizeof(path), nullptr, nullptr);
    std::strncat(path, "scenario_disrec_test.disrec", sizeof(path) - std::strlen(path) - 1);

    std::vector<std::vector<unsigned char>> originals;
    for (int i = 0; i < 100; ++i) {
        std::vector<unsigned char> rec(7 + (i % 32));
        for (size_t k = 0; k < rec.size(); ++k)
            rec[k] = static_cast<unsigned char>((i * 31 + k) & 0xFF);
        originals.push_back(std::move(rec));
    }

    {
        DisrecWriter w;
        if (!w.Open(path)) { std::printf("FAIL open(write)\n"); return 1; }
        for (size_t i = 0; i < originals.size(); ++i) {
            const uint64_t ts = static_cast<uint64_t>(i) * 200000ULL; // 5 Hz spacing
            if (!w.WriteRecord(ts, originals[i].data(), originals[i].size())) {
                std::printf("FAIL writeRecord %zu\n", i); return 1;
            }
        }
    }

    {
        DisrecReader r;
        if (!r.Open(path)) { std::printf("FAIL open(read)\n"); return 1; }
        for (size_t i = 0; i < originals.size(); ++i) {
            uint64_t ts = 0;
            std::vector<unsigned char> back;
            if (!r.NextRecord(ts, back)) {
                std::printf("FAIL nextRecord %zu (EOF)\n", i); return 1;
            }
            if (ts != static_cast<uint64_t>(i) * 200000ULL) {
                std::printf("FAIL ts mismatch %zu\n", i); return 1;
            }
            if (back != originals[i]) {
                std::printf("FAIL bytes mismatch %zu\n", i); return 1;
            }
        }
        uint64_t ts = 0;
        std::vector<unsigned char> back;
        if (r.NextRecord(ts, back)) {
            std::printf("FAIL: read past end succeeded\n"); return 1;
        }
    }

    std::printf("disrec_roundtrip PASS\n");
    return 0;
}
