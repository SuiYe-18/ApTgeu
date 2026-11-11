// v0.16: minimal PE32 stub generator - just shows "needs 64-bit Windows" messagebox
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static void put16(std::vector<uint8_t>& d, size_t off, uint16_t v) {
    d[off] = v & 0xFF; d[off+1] = (v >> 8) & 0xFF;
}
static void put32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
    d[off] = v & 0xFF; d[off+1] = (v>>8)&0xFF; d[off+2]=(v>>16)&0xFF; d[off+3]=(v>>24)&0xFF;
}

bool write_32bit_stub(const std::string& path) {
    // Layout:
    // 0x000-0x1FF: headers (512 bytes)
    // 0x200-0x3FF: .text (code, RVA 0x1000)
    // 0x400-0x5FF: .idata (imports + strings, RVA 0x2000)
    std::vector<uint8_t> d(0x600, 0);

    // ---- DOS header ----
    d[0x00] = 'M'; d[0x01] = 'Z';
    put32(d, 0x3C, 0x40);  // e_lfanew

    // ---- PE signature ----
    d[0x40] = 'P'; d[0x41] = 'E'; d[0x42] = 0; d[0x43] = 0;

    // ---- COFF header (0x44, 20 bytes) ----
    put16(d, 0x44, 0x014C);  // Machine = i386
    put16(d, 0x46, 2);       // NumberOfSections
    put32(d, 0x48, 0);       // TimeDateStamp
    put32(d, 0x4C, 0);       // PointerToSymbolTable
    put32(d, 0x50, 0);       // NumberOfSymbols
    put16(d, 0x54, 0xE0);    // SizeOfOptionalHeader = 224
    put16(d, 0x56, 0x0102);  // Characteristics: EXECUTABLE_IMAGE | 32BIT_MACHINE

    // ---- Optional Header32 (0x58, 224 bytes) ----
    put16(d, 0x58, 0x010B);  // Magic = PE32
    d[0x5A] = 6; d[0x5B] = 0;  // linker version
    put32(d, 0x5C, 0x200);   // SizeOfCode
    put32(d, 0x60, 0x200);   // SizeOfInitializedData
    put32(d, 0x64, 0);       // SizeOfUninitializedData
    put32(d, 0x68, 0x1000);  // AddressOfEntryPoint
    put32(d, 0x6C, 0x1000);  // BaseOfCode
    put32(d, 0x70, 0x2000);  // BaseOfData
    put32(d, 0x74, 0x00400000);  // ImageBase
    put32(d, 0x78, 0x1000);  // SectionAlignment
    put32(d, 0x7C, 0x200);   // FileAlignment
    put16(d, 0x80, 4); put16(d, 0x82, 0);  // OS version
    put16(d, 0x84, 0); put16(d, 0x86, 0);  // Image version
    put16(d, 0x88, 4); put16(d, 0x8A, 0);  // Subsystem version
    put32(d, 0x8C, 0);       // Win32VersionValue
    put32(d, 0x90, 0x4000);  // SizeOfImage
    put32(d, 0x94, 0x200);   // SizeOfHeaders
    put32(d, 0x98, 0);       // CheckSum
    put16(d, 0x9C, 2);       // Subsystem = GUI
    put16(d, 0x9E, 0);       // DllCharacteristics
    put32(d, 0xA0, 0x100000); // SizeOfStackReserve
    put32(d, 0xA4, 0x1000);   // SizeOfStackCommit
    put32(d, 0xA8, 0x100000); // SizeOfHeapReserve
    put32(d, 0xAC, 0x1000);   // SizeOfHeapCommit
    put32(d, 0xB0, 0);       // LoaderFlags
    put32(d, 0xB4, 16);      // NumberOfRvaAndSizes
    // Data directories (0xB8 onward, 16 * 8 = 128 bytes)
    // dir[1] = Import Table at RVA 0x2000, size 40
    put32(d, 0xB8 + 1*8 + 0, 0x2000);  // Import RVA
    put32(d, 0xB8 + 1*8 + 4, 40);      // Import Size

    // ---- Section table (0x138, 2 sections * 40 = 80 bytes) ----
    // .text
    memcpy(&d[0x138], ".text", 5);
    put32(d, 0x138+8, 0x200);    // VirtualSize
    put32(d, 0x138+12, 0x1000);  // VirtualAddress
    put32(d, 0x138+16, 0x200);   // SizeOfRawData
    put32(d, 0x138+20, 0x200);   // PointerToRawData
    put32(d, 0x138+36, 0x60000020);  // CODE|EXEC|READ
    // .rdata
    memcpy(&d[0x160], ".rdata", 7);
    put32(d, 0x160+8, 0x200);    // VirtualSize
    put32(d, 0x160+12, 0x2000);  // VirtualAddress
    put32(d, 0x160+16, 0x200);   // SizeOfRawData
    put32(d, 0x160+20, 0x400);   // PointerToRawData
    put32(d, 0x160+36, 0x40000040);  // INITIALIZED_DATA|READ|WRITE

    // ---- .idata content (file offset 0x400, RVA 0x2000) ----
    // Strings placed first for easy offsets:
    // 0x400: "user32.dll\0" (12 bytes) -> RVA 0x2000
    // 0x40C: "kernel32.dll\0" (13 bytes) -> RVA 0x200C
    // 0x419: hint+name "MessageBoxW\0" (14 bytes, 2-byte hint prefix) -> RVA 0x2019
    // 0x427: hint+name "ExitProcess\0" (13 bytes) -> RVA 0x2027
    // 0x436: wide string text -> RVA 0x2036
    // 0x460: wide string caption -> RVA 0x2060

    const char* user32 = "user32.dll";
    memcpy(&d[0x400], user32, 12);
    const char* kernel32 = "kernel32.dll";
    memcpy(&d[0x40C], kernel32, 13);

    // hint = 0, then name
    put16(d, 0x419, 0);
    memcpy(&d[0x41B], "MessageBoxW", 11); d[0x426] = 0;

    put16(d, 0x427, 0);
    memcpy(&d[0x429], "ExitProcess", 11); d[0x434] = 0;

    // Wide strings
    const wchar_t* text = L"此安装包需要 64 位 Windows。";
    const wchar_t* caption = L"ApTgeu Setup";
    memcpy(&d[0x436], text, (wcslen(text)+1)*2);
    size_t cap_off = 0x436 + ((wcslen(text)+1)*2 + 15) & ~15;  // align
    memcpy(&d[cap_off], caption, (wcslen(caption)+1)*2);

    // IAT/INT layout:
    // We need 2 import descriptors (40 bytes), then INTs and IATs.
    // Place import descriptors at RVA 0x2080 (file 0x480):
    //   desc[0]: user32 -> OriginalFirstThunk=0x20A0, Name=0x2000, FirstThunk=0x20B0
    //   desc[1]: kernel32 -> OriginalFirstThunk=0x20C0, Name=0x200C, FirstThunk=0x20D0
    //   desc[2]: all zeros (null terminator)
    // INT[0] at RVA 0x20A0: [0x2019 (MessageBoxW), 0]
    // IAT[0] at RVA 0x20B0: [0x2019, 0]  (loader fills these)
    // INT[1] at RVA 0x20C0: [0x2027 (ExitProcess), 0]
    // IAT[1] at RVA 0x20D0: [0x2027, 0]

    size_t id_base = 0x480;  // file offset
    // desc[0] user32
    put32(d, id_base+0, 0x20A0);  // OriginalFirstThunk
    put32(d, id_base+4, 0);       // TimeDateStamp
    put32(d, id_base+8, 0);       // ForwarderChain
    put32(d, id_base+12, 0x2000); // Name
    put32(d, id_base+16, 0x20B0); // FirstThunk
    // desc[1] kernel32
    put32(d, id_base+20, 0x20C0);
    put32(d, id_base+24, 0);
    put32(d, id_base+28, 0);
    put32(d, id_base+32, 0x200C);
    put32(d, id_base+36, 0x20D0);
    // desc[2] null = already zeros

    // INT[0] at file 0x4A0 (RVA 0x20A0)
    put32(d, 0x4A0, 0x2019);
    put32(d, 0x4A4, 0);
    // IAT[0] at file 0x4B0 (RVA 0x20B0)
    put32(d, 0x4B0, 0x2019);
    put32(d, 0x4B4, 0);
    // INT[1] at file 0x4C0 (RVA 0x20C0)
    put32(d, 0x4C0, 0x2027);
    put32(d, 0x4C4, 0);
    // IAT[1] at file 0x4D0 (RVA 0x20D0)
    put32(d, 0x4D0, 0x2027);
    put32(d, 0x4D4, 0);

    // ---- .text machine code (file offset 0x200, RVA 0x1000) ----
    // MessageBoxW(NULL, text, caption, MB_OK=0)
    // push 0                          ; uType
    // push RVA(caption)+ImageBase     ; lpCaption
    // push RVA(text)+ImageBase        ; lpText
    // push 0                          ; hWnd
    // call [IAT_MessageBoxW] = [0x400000 + 0x20B0]
    // push 0
    // call [IAT_ExitProcess] = [0x400000 + 0x20D0]
    std::vector<uint8_t> code;
    // push 0
    code.push_back(0x6A); code.push_back(0x00);
    // push imm32 (caption VA = 0x400000 + 0x2000 + (cap_off - 0x400))
    uint32_t caption_va = 0x400000 + 0x2000 + (uint32_t)(cap_off - 0x400);
    code.push_back(0x68);
    code.push_back(caption_va & 0xFF); code.push_back((caption_va>>8)&0xFF);
    code.push_back((caption_va>>16)&0xFF); code.push_back((caption_va>>24)&0xFF);
    // push imm32 (text VA = 0x400000 + 0x2036)
    uint32_t text_va = 0x400000 + 0x2036;
    code.push_back(0x68);
    code.push_back(text_va & 0xFF); code.push_back((text_va>>8)&0xFF);
    code.push_back((text_va>>16)&0xFF); code.push_back((text_va>>24)&0xFF);
    // push 0
    code.push_back(0x6A); code.push_back(0x00);
    // call [imm32] = MessageBoxW IAT
    code.push_back(0xFF); code.push_back(0x15);
    uint32_t mbw_iat = 0x400000 + 0x20B0;
    code.push_back(mbw_iat & 0xFF); code.push_back((mbw_iat>>8)&0xFF);
    code.push_back((mbw_iat>>16)&0xFF); code.push_back((mbw_iat>>24)&0xFF);
    // push 0
    code.push_back(0x6A); code.push_back(0x00);
    // call [imm32] = ExitProcess IAT
    code.push_back(0xFF); code.push_back(0x15);
    uint32_t ep_iat = 0x400000 + 0x20D0;
    code.push_back(ep_iat & 0xFF); code.push_back((ep_iat>>8)&0xFF);
    code.push_back((ep_iat>>16)&0xFF); code.push_back((ep_iat>>24)&0xFF);

    memcpy(&d[0x200], code.data(), code.size());

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
    return true;
}
