/*
 * ApTgeu 32-bit minimal stub - pure Win32 C, no STL, no UI popups.
 * Reads payload from end of own exe, writes files to /DIR=
 * Per-file LZMA2 decompression when compression=1.
 *
 * Per-file data layout for comp=1:
 *   byte     prop              (LZMA2 properties byte)
 *   uint64   uncomp_size LE    (original file size)
 *   byte[]   lzma2_stream      (raw LZMA2 stream, end-marked)
 * For comp=0: raw_size bytes of raw file data.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <lmcons.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "Lzma2Dec.h"
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t rd_u64(const uint8_t* p) {
    uint64_t lo = rd_u32(p);
    uint64_t hi = rd_u32(p+4);
    return lo | (hi << 32);
}

static void* LzmaAlloc(ISzAllocPtr, size_t sz) { return malloc(sz); }
static void  LzmaFree(ISzAllocPtr, void* p)     { free(p); }
static ISzAlloc g_lzma = { &LzmaAlloc, &LzmaFree };

// Convert UTF-8 rel path to wide, '/' -> '\\', append to out buffer.
// out must have room for MAX_PATH. Returns length in wchar_t (excl NUL).
static int rel_to_wide(const char* src, int srclen, wchar_t* out, int outcap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, srclen, out, outcap);
    if (n <= 0) { out[0] = 0; return 0; }
    for (int i = 0; i < n; i++) if (out[i] == L'/') out[i] = L'\\';
    out[n] = 0;
    return n;
}

static void create_parent_dirs(const wchar_t* full) {
    // Find last backslash.
    wchar_t tmp[MAX_PATH];
    int n = lstrlenW(full);
    int slash = -1;
    for (int i = 0; i < n; i++) if (full[i] == L'\\') slash = i;
    if (slash < 2) return;
    lstrcpynW(tmp, full, MAX_PATH);
    tmp[slash] = 0;
    // tmp = parent. Walk and CreateDirectoryW at each separator.
    for (int i = 3; i < (int)lstrlenW(tmp); i++) {
        if (tmp[i] == L'\\') {
            wchar_t save = tmp[i];
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = save;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Parse /DIR=
    wchar_t dir[MAX_PATH] = L"C:\\Temp\\ApTgeu_32bit";
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; i++) {
                if (wcsncmp(argv[i], L"/DIR=", 5) == 0) {
                    const wchar_t* v = argv[i] + 5;
                    int vl = lstrlenW(v);
                    if (vl >= 2 && v[0] == L'"' && v[vl-1] == L'"') {
                        wcsncpy(dir, v + 1, vl - 2);
                        dir[vl - 2] = 0;
                    } else {
                        wcsncpy(dir, v, MAX_PATH - 1);
                        dir[MAX_PATH - 1] = 0;
                    }
                }
            }
            LocalFree(argv);
        }
    }

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) ExitProcess(1);
    DWORD fsize = GetFileSize(h, NULL);

    SetFilePointer(h, (LONG)(fsize - 28), NULL, FILE_BEGIN);
    uint8_t footer[28];
    DWORD rd = 0;
    ReadFile(h, footer, 28, &rd, NULL);

    if (memcmp(footer, "APTGEU06", 8) != 0 && memcmp(footer, "APTGEU03", 8) != 0) {
        CloseHandle(h); ExitProcess(2);
    }

    uint64_t payload_offset = rd_u64(footer + 8);
    uint64_t total_payload  = rd_u64(footer + 16);
    if (total_payload == 0 || payload_offset >= fsize) {
        CloseHandle(h); ExitProcess(3);
    }

    uint8_t* payload = (uint8_t*)malloc((size_t)total_payload);
    if (!payload) { CloseHandle(h); ExitProcess(4); }
    SetFilePointer(h, (LONG)payload_offset, NULL, FILE_BEGIN);
    ReadFile(h, payload, (DWORD)total_payload, &rd, NULL);
    CloseHandle(h);

    uint32_t pos = 0;
    uint32_t plen = (uint32_t)total_payload;

    if (pos + 4 > plen) { free(payload); ExitProcess(5); }
    uint32_t config_len = rd_u32(payload + pos); pos += 4 + config_len;
    if (pos + 4 > plen) { free(payload); ExitProcess(6); }
    uint32_t ui_len = rd_u32(payload + pos); pos += 4 + ui_len;
    if (pos + 4 > plen) { free(payload); ExitProcess(7); }
    uint32_t file_count = rd_u32(payload + pos); pos += 4;

    CreateDirectoryW(dir, NULL);

    int written = 0;

    for (uint32_t i = 0; i < file_count; i++) {
        if (pos + 4 > plen) { free(payload); ExitProcess(8); }
        uint32_t rp_len = rd_u32(payload + pos); pos += 4;
        if (pos + rp_len > plen) { free(payload); ExitProcess(9); }
        const char* rel = (const char*)(payload + pos);
        pos += rp_len;

        if (pos + 4 > plen) { free(payload); ExitProcess(10); }
        uint32_t comp_len = rd_u32(payload + pos); pos += 4 + comp_len;
        if (pos + 4 > plen) { free(payload); ExitProcess(11); }
        uint32_t tasks_len = rd_u32(payload + pos); pos += 4 + tasks_len;
        if (pos + 4 > plen) { free(payload); ExitProcess(12); }
        uint32_t flags_len = rd_u32(payload + pos); pos += 4 + flags_len;
        if (pos + 9 > plen) { free(payload); ExitProcess(13); }
        uint32_t raw_size = rd_u32(payload + pos); pos += 4;
        pos += 4; // crc
        uint8_t comp = payload[pos]; pos += 1;

        if (pos + raw_size > plen) { free(payload); ExitProcess(14); }

        // Build full path: dir + "\" + wide(rel)
        wchar_t wrel[MAX_PATH];
        rel_to_wide(rel, rp_len, wrel, MAX_PATH - 2);
        wchar_t full[MAX_PATH];
        lstrcpyW(full, dir);
        int dl = lstrlenW(full);
        if (dl > 0 && full[dl-1] != L'\\') { full[dl] = L'\\'; full[dl+1] = 0; }
        lstrcatW(full, wrel);

        create_parent_dirs(full);

        HANDLE hf = CreateFileW(full, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) { pos += raw_size; continue; }

        bool ok = false;
        if (comp == 0) {
            DWORD w = 0;
            WriteFile(hf, payload + pos, raw_size, &w, NULL);
            ok = (w == raw_size);
        } else {
            if (raw_size >= 9) {
                Byte prop = payload[pos];
                uint64_t uncomp_size = rd_u64(payload + pos + 1);
                const Byte* stream = payload + pos + 9;
                SizeT stream_len = (SizeT)(raw_size - 9);

                uint8_t* outbuf = (uint8_t*)malloc((size_t)uncomp_size);
                if (outbuf) {
                    SizeT dest_len = (SizeT)uncomp_size;
                    SizeT src_len  = stream_len;
                    ELzmaStatus status;
                    SRes rc = Lzma2Decode(outbuf, &dest_len,
                                          stream, &src_len,
                                          prop, LZMA_FINISH_END, &status,
                                          &g_lzma);
                    if (rc == SZ_OK && status == LZMA_STATUS_FINISHED_WITH_MARK) {
                        DWORD w = 0;
                        WriteFile(hf, outbuf, (DWORD)dest_len, &w, NULL);
                        ok = (w == (DWORD)dest_len);
                    }
                    free(outbuf);
                }
            }
        }
        CloseHandle(hf);
        if (ok) written++;
        pos += raw_size;
    }

    free(payload);
    ExitProcess(0);
    return 0;
}
