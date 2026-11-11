/* Minimal PE32/PE32+ writer - builds a valid Windows EXE from scratch.
 * Based on the CBBC pe.c approach: linear buffer, append headers + sections. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PE_MAX_SECTIONS 16
#define PE_IMP_NAME_MAX 64
#define PE_IMP_FUNC_MAX 64

typedef struct {
    uint16_t machine;       /* 0x14C=i386, 0x8664=amd64 */
    uint16_t subsystem;     /* 2=GUI, 3=CUI */
    uint32_t image_base;
    uint32_t entry_rva;

    /* sections */
    struct {
        char     name[8];
        uint32_t vsize;
        uint32_t vsize_raw; /* same as vsize unless uninit data */
        uint32_t raw_offset;
        uint32_t characteristics;
        const uint8_t* data;
        uint32_t data_len;
    } sects[PE_MAX_SECTIONS];
    int nsects;

    /* imports: flat list of dll+func pairs */
    struct {
        char dll[PE_IMP_NAME_MAX];
        char func[PE_IMP_FUNC_MAX];
    } imports[128];
    int nimports;

    uint8_t* buf;
    uint32_t cap;
    uint32_t size;
} pe_writer;

void  pe_init(pe_writer* w, uint16_t machine, uint16_t subsystem);
int   pe_add_section(pe_writer* w, const char* name, const uint8_t* data, uint32_t len, uint32_t chars);
int   pe_add_import(pe_writer* w, const char* dll, const char* func);
void  pe_set_entry(pe_writer* w, uint32_t rva);
/* Builds the PE into w->buf. Returns final size, 0 on error. */
uint32_t pe_build(pe_writer* w);
void  pe_free(pe_writer* w);
