#include "pe_writer.h"
#include <stdlib.h>
#include <string.h>

static void wr8(uint8_t* p, uint8_t v) { p[0] = v; }
static void wr16(uint8_t* p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void wr32(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

void pe_init(pe_writer* w, uint16_t machine, uint16_t subsystem) {
    memset(w, 0, sizeof(*w));
    w->machine = machine;
    w->subsystem = subsystem;
    w->image_base = 0x00400000;
    w->cap = 65536;
    w->buf = (uint8_t*)malloc(w->cap);
    memset(w->buf, 0, w->cap);
}

int pe_add_section(pe_writer* w, const char* name, const uint8_t* data, uint32_t len, uint32_t chars) {
    if (w->nsects >= PE_MAX_SECTIONS) return -1;
    int i = w->nsects++;
    strncpy(w->sects[i].name, name, 7);
    w->sects[i].data = data;
    w->sects[i].data_len = len;
    w->sects[i].characteristics = chars;
    w->sects[i].vsize = len;
    return i;
}

int pe_add_import(pe_writer* w, const char* dll, const char* func) {
    if (w->nimports >= 128) return -1;
    strncpy(w->imports[w->nimports].dll, dll, PE_IMP_NAME_MAX-1);
    strncpy(w->imports[w->nimports].func, func, PE_IMP_FUNC_MAX-1);
    return w->nimports++;
}

void pe_set_entry(pe_writer* w, uint32_t rva) { w->entry_rva = rva; }

void pe_free(pe_writer* w) { free(w->buf); w->buf = 0; }

#define FILE_ALIGN 0x200
#define SECT_ALIGN 0x1000

uint32_t pe_build(pe_writer* w) {
    int is_pe32plus = (w->machine == 0x8664);
    int opt_hdr_size = is_pe32plus ? 240 : 224;
    int num_sects = w->nsects;

    // Header size: DOS(64) + PE(4) + COFF(20) + OptHdr + SectTable
    uint32_t headers_size = 64 + 4 + 20 + opt_hdr_size + num_sects * 40;
    // Align up to FILE_ALIGN
    headers_size = (headers_size + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1);

    // Calculate raw offsets and RVA for each section
    uint32_t raw_off = headers_size;
    uint32_t rva = SECT_ALIGN;
    for (int i = 0; i < num_sects; i++) {
        w->sects[i].raw_offset = raw_off;
        w->sects[i].vsize = (w->sects[i].data_len + SECT_ALIGN - 1) & ~(SECT_ALIGN - 1);
        w->sects[i].vsize_raw = w->sects[i].data_len;
        raw_off = (raw_off + w->sects[i].data_len + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1);
        rva += SECT_ALIGN;
    }
    uint32_t image_size = rva; // total SizeOfImage

    // Total file size
    uint32_t total = raw_off;
    if (total > w->cap) { w->buf = (uint8_t*)realloc(w->buf, total); w->cap = total; }
    memset(w->buf, 0, total);

    // ---- DOS header ----
    wr8(w->buf+0x00, 'M'); wr8(w->buf+0x01, 'Z');
    wr32(w->buf+0x3C, 0x40);

    // ---- PE signature ----
    wr8(w->buf+0x40,'P'); wr8(w->buf+0x41,'E'); wr8(w->buf+0x42,0); wr8(w->buf+0x43,0);

    // ---- COFF header ----
    uint8_t* coff = w->buf + 0x44;
    wr16(coff+0, w->machine);
    wr16(coff+2, num_sects);
    wr32(coff+4, 0);   // timestamp
    wr32(coff+8, 0);   // symtab
    wr32(coff+12, 0);  // nsyms
    wr16(coff+16, opt_hdr_size);
    wr16(coff+18, 0x0102); // EXECUTABLE_IMAGE | 32BIT_MACHINE (if i386)

    // ---- Optional header ----
    uint8_t* opt = coff + 20;
    wr16(opt+0, is_pe32plus ? 0x020B : 0x010B); // magic
    wr8(opt+2, 14); wr8(opt+3, 0); // linker
    // SizeOfCode / SizeOfInitializedData filled later
    wr32(opt+16, w->entry_rva);
    wr32(opt+12, SECT_ALIGN); // BaseOfCode
    if (!is_pe32plus) wr32(opt+16-4+4, 0); // BaseOfData (PE32 only)
    // ImageBase
    if (is_pe32plus) {
        // PE32+: ImageBase at offset 24, 8 bytes
        wr32(opt+24, w->image_base & 0xFFFFFFFF);
        wr32(opt+28, (w->image_base >> 32) & 0xFFFFFFFF);
    } else {
        wr32(opt+28, w->image_base);
    }
    wr32(opt+32, SECT_ALIGN);  // SectionAlignment
    wr32(opt+36, FILE_ALIGN);  // FileAlignment
    wr16(opt+40, 4); wr16(opt+42, 0); // OS version
    wr16(opt+44, 0); wr16(opt+46, 0); // Image version
    wr16(opt+48, 4); wr16(opt+50, 0); // Subsystem version
    wr32(opt+52, 0);  // Win32Version
    wr32(opt+56, image_size);  // SizeOfImage
    wr32(opt+60, headers_size); // SizeOfHeaders
    wr32(opt+64, 0);  // CheckSum
    wr16(opt+68, w->subsystem); // Subsystem
    wr16(opt+70, 0);  // DllCharacteristics
    if (is_pe32plus) {
        wr64:;
        wr32(opt+72, 0x100000); // StackReserve lo
        wr32(opt+76, 0);
        wr32(opt+80, 0x1000);   // StackCommit
        wr32(opt+84, 0);
        wr32(opt+88, 0x100000); // HeapReserve
        wr32(opt+92, 0);
        wr32(opt+96, 0x1000);   // HeapCommit
        wr32(opt+100, 0);
        wr32(opt+104, 0);       // LoaderFlags
        wr32(opt+108, 16);      // NumberOfRvaAndSizes
    } else {
        wr32(opt+72, 0x100000); // StackReserve
        wr32(opt+76, 0x1000);   // StackCommit
        wr32(opt+80, 0x100000); // HeapReserve
        wr32(opt+84, 0x1000);   // HeapCommit
        wr32(opt+88, 0);        // LoaderFlags
        wr32(opt+92, 16);       // NumberOfRvaAndSizes
    }

    // Data directories: dir[1] = Import
    // We need to know import dir RVA. Put it in the last section (.rdata).
    // For simplicity, compute import layout at end of .rdata section.
    // Find .rdata section
    int rdata_idx = -1;
    for (int i = 0; i < num_sects; i++) {
        if (strcmp(w->sects[i].name, ".rdata") == 0) { rdata_idx = i; break; }
    }

    uint32_t import_dir_rva = 0;
    uint32_t import_dir_size = 0;
    if (rdata_idx >= 0 && w->nimports > 0) {
        // Place import structures at end of .rdata raw data
        uint32_t rd_raw = w->sects[rdata_idx].raw_offset;
        uint32_t rd_rva_base = SECT_ALIGN * (rdata_idx + 1);
        uint32_t rd_len = w->sects[rdata_idx].data_len;
        uint32_t p = rd_raw + rd_len; // append after existing data

        // We'll build: dll names, func names (hint+name), INT, IAT, import dir
        // First pass: compute offsets
        // String pool: each dll name\0, each func name\0 preceded by hint(2)
        // Group imports by dll
        int ndlls = 0;
        char dll_names[128][PE_IMP_NAME_MAX];
        int dll_func_start[128];
        int dll_func_count[128];
        for (int i = 0; i < w->nimports; i++) {
            int found = -1;
            for (int d = 0; d < ndlls; d++) {
                if (strcmp(dll_names[d], w->imports[i].dll) == 0) { found = d; break; }
            }
            if (found < 0) {
                found = ndlls++;
                strcpy(dll_names[found], w->imports[i].dll);
                dll_func_start[found] = i;
                dll_func_count[found] = 0;
            }
            dll_func_count[found]++;
        }

        // Layout in .rdata:
        // 1. dll name strings (each "name.dll\0")
        // 2. hint/name entries for each import (2-byte hint + name\0, aligned to even)
        // 3. INT arrays (one per dll, terminated by 0)
        // 4. IAT arrays (one per dll, terminated by 0)
        // 5. Import directory (ndlls+1 entries, 20 bytes each)

        uint32_t pool = p;
        // dll names
        uint32_t dll_name_rva[128];
        for (int d = 0; d < ndlls; d++) {
            dll_name_rva[d] = rd_rva_base + (pool - rd_raw);
            int nl = strlen(dll_names[d]) + 1;
            memcpy(w->buf + pool, dll_names[d], nl);
            pool += nl;
        }
        // func hint/name
        uint32_t func_hint_rva[128];
        for (int i = 0; i < w->nimports; i++) {
            func_hint_rva[i] = rd_rva_base + (pool - rd_raw);
            wr16(w->buf + pool, 0); // hint
            pool += 2;
            int nl = strlen(w->imports[i].func) + 1;
            memcpy(w->buf + pool, w->imports[i].func, nl);
            pool += nl;
            if (pool % 2) pool++; // align
        }
        // INT arrays
        uint32_t int_rva[128];
        for (int d = 0; d < ndlls; d++) {
            int_rva[d] = rd_rva_base + (pool - rd_raw);
            for (int f = 0; f < dll_func_count[d]; f++) {
                wr32(w->buf + pool, func_hint_rva[dll_func_start[d] + f]);
                pool += 4;
            }
            wr32(w->buf + pool, 0);
            pool += 4;
        }
        // IAT arrays
        uint32_t iat_rva[128];
        for (int d = 0; d < ndlls; d++) {
            iat_rva[d] = rd_rva_base + (pool - rd_raw);
            for (int f = 0; f < dll_func_count[d]; f++) {
                wr32(w->buf + pool, func_hint_rva[dll_func_start[d] + f]);
                pool += 4;
            }
            wr32(w->buf + pool, 0);
            pool += 4;
        }
        // Import directory
        import_dir_rva = rd_rva_base + (pool - rd_raw);
        for (int d = 0; d < ndlls; d++) {
            wr32(w->buf + pool, int_rva[d]);   // OriginalFirstThunk
            wr32(w->buf + pool+4, 0);          // TimeDateStamp
            wr32(w->buf + pool+8, 0);          // ForwarderChain
            wr32(w->buf + pool+12, dll_name_rva[d]); // Name
            wr32(w->buf + pool+16, iat_rva[d]); // FirstThunk
            pool += 20;
        }
        // null descriptor
        memset(w->buf + pool, 0, 20);
        pool += 20;
        import_dir_size = pool - (rd_raw + rd_len);

        // Update .rdata data_len to include appended import data
        w->sects[rdata_idx].data_len = pool - rd_raw;
    }

    // Write data directory[1] = Import
    uint32_t dir_off = is_pe32plus ? 112 : 96;
    wr32(opt + dir_off + 1*8 + 0, import_dir_rva);
    wr32(opt + dir_off + 1*8 + 4, import_dir_size);

    // ---- Section table ----
    uint8_t* sect = opt + opt_hdr_size;
    for (int i = 0; i < num_sects; i++) {
        memset(sect, 0, 40);
        strncpy((char*)sect, w->sects[i].name, 7);
        wr32(sect+8, w->sects[i].vsize_raw);
        wr32(sect+12, SECT_ALIGN * (i+1)); // VirtualAddress
        wr32(sect+16, (w->sects[i].data_len + FILE_ALIGN - 1) & ~(FILE_ALIGN-1)); // SizeOfRawData
        wr32(sect+20, w->sects[i].raw_offset);
        wr32(sect+36, w->sects[i].characteristics);
        sect += 40;
    }

    // ---- Section data ----
    for (int i = 0; i < num_sects; i++) {
        if (w->sects[i].data && w->sects[i].data_len > 0) {
            memcpy(w->buf + w->sects[i].raw_offset, w->sects[i].data, w->sects[i].data_len);
        }
    }

    w->size = total;
    return total;
}
