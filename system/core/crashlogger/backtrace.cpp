// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <elf.h>
#include <inttypes.h>
#include <limits.h>
#include <link.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <magenta/types.h>
#include <magenta/syscalls.h>

#include "backtrace.h"

extern struct r_debug* _dl_debug_addr;

#define MAX_BUILDID_SIZE 64

typedef struct dsoinfo dsoinfo_t;
struct dsoinfo {
    dsoinfo_t* next;
    uintptr_t base;
    char buildid[MAX_BUILDID_SIZE * 2 + 1];
    char name[];
};

static dsoinfo_t* dsolist_add(dsoinfo_t** list, const char* name, uintptr_t base) {
    if (!strcmp(name, "libc.so")) {
        name = "libmusl.so";
    }
    size_t len = strlen(name);
    auto dso = reinterpret_cast<dsoinfo_t*> (calloc(1, sizeof(dsoinfo_t) + len + 1));
    if (dso == nullptr) {
        return nullptr;
    }
    memcpy(dso->name, name, len + 1);
    memset(dso->buildid, 'x', sizeof(dso->buildid) - 1);
    dso->base = base;
    while (*list != nullptr) {
        if ((*list)->base < dso->base) {
            dso->next = *list;
            *list = dso;
            return dso;
        }
        list = &((*list)->next);
    }
    *list = dso;
    dso->next = nullptr;
    return dso;
}

#define rdebug_vaddr ((uintptr_t) _dl_debug_addr)
#define rdebug_off_lmap offsetof(struct r_debug, r_map)

#define lmap_off_next offsetof(struct link_map, l_next)
#define lmap_off_name offsetof(struct link_map, l_name)
#define lmap_off_addr offsetof(struct link_map, l_addr)

static inline mx_status_t read_mem(mx_handle_t h, mx_vaddr_t vaddr, void* ptr, size_t len) {
    mx_ssize_t bytes_read = mx_debug_read_memory(h, vaddr, len, ptr);
    if (bytes_read < 0) {
        printf("read_mem @%p FAILED %d\n", (void*) vaddr, (int) bytes_read);
        return (mx_status_t) bytes_read;
    }
    if (bytes_read != (mx_ssize_t) len) {
        printf("read_mem @%p FAILED, short read %ld\n", (void*) vaddr, (long) bytes_read);
        return ERR_IO;
    }
    return NO_ERROR;
}

static mx_status_t fetch_string(mx_handle_t h, mx_vaddr_t vaddr, char* ptr, size_t max) {
    while (max > 1) {
        mx_status_t status;
        if ((status = read_mem(h, vaddr, ptr, 1)) < 0) {
            *ptr = 0;
            return status;
        }
        ptr++;
        vaddr++;
        max--;
    }
    *ptr = 0;
    return NO_ERROR;
}

#if UINT_MAX == ULONG_MAX

#define ehdr_off_phoff offsetof(Elf32_Ehdr, e_phoff)
#define ehdr_off_phnum offsetof(Elf32_Ehdr, e_phnum)

#define phdr_off_type offsetof(Elf32_Phdr, p_type)
#define phdr_off_offset offsetof(Elf32_Phdr, p_offset)
#define phdr_off_filesz offsetof(Elf32_Phdr, p_filesz)

typedef Elf32_Half elf_half_t;
typedef Elf32_Off elf_off_t;
// ELF used "word" for 32 bits, sigh.
typedef Elf32_Word elf_word_t;
typedef Elf32_Word elf_native_word_t;

#else

#define ehdr_off_phoff offsetof(Elf64_Ehdr, e_phoff)
#define ehdr_off_phnum offsetof(Elf64_Ehdr, e_phnum)

#define phdr_off_type offsetof(Elf64_Phdr, p_type)
#define phdr_off_offset offsetof(Elf64_Phdr, p_offset)
#define phdr_off_filesz offsetof(Elf64_Phdr, p_filesz)

typedef Elf64_Half elf_half_t;
typedef Elf64_Off elf_off_t;
typedef Elf64_Word elf_word_t;
typedef Elf64_Xword elf_native_word_t;

#endif

void fetch_build_id(mx_handle_t h, dsoinfo_t* dso) {
    mx_vaddr_t vaddr = dso->base;
    uint8_t tmp[4];
    if (read_mem(h, vaddr, tmp, 4) ||
        memcmp(tmp, ELFMAG, SELFMAG)) {
        return;
    }
    elf_off_t phoff;
    elf_half_t num;
    if (read_mem(h, vaddr + ehdr_off_phoff, &phoff, sizeof(phoff)) ||
        read_mem(h, vaddr + ehdr_off_phnum, &num, sizeof(num))) {
        return;
    }
    for (unsigned n = 0; n < num; n++) {
        mx_vaddr_t phaddr = vaddr + phoff + (n * sizeof(Elf64_Phdr));
        elf_word_t type;
        if (read_mem(h, phaddr + phdr_off_type, &type, sizeof(type))) {
            return;
        }
        if (type != PT_NOTE) {
            continue;
        }
        elf_off_t off;
        elf_native_word_t size;
        if (read_mem(h, phaddr + phdr_off_offset, &off, sizeof(off)) ||
            read_mem(h, phaddr + phdr_off_filesz, &size, sizeof(size))) {
            return;
        }
        struct {
            Elf32_Nhdr hdr;
            char name[sizeof("GNU")];
        } hdr;
        while (size > sizeof(hdr)) {
            if (read_mem(h, vaddr + off, &hdr, sizeof(hdr))) {
                return;
            }
            size_t header_size =
                sizeof(Elf32_Nhdr) + ((hdr.hdr.n_namesz + 3) & -4);
            size_t payload_size = (hdr.hdr.n_descsz + 3) & -4;
            off += header_size;
            size -= header_size;
            mx_vaddr_t payload_vaddr = vaddr + off;
            off += payload_size;
            size -= payload_size;
            if (hdr.hdr.n_type != NT_GNU_BUILD_ID ||
                hdr.hdr.n_namesz != sizeof("GNU") ||
                memcmp(hdr.name, "GNU", sizeof("GNU")) != 0) {
                continue;
            }
            if (hdr.hdr.n_descsz > MAX_BUILDID_SIZE) {
                snprintf(dso->buildid, sizeof(dso->buildid),
                         "build_id_too_large_%u", hdr.hdr.n_descsz);
            } else {
                uint8_t buildid[MAX_BUILDID_SIZE];
                if (read_mem(h, payload_vaddr, buildid, hdr.hdr.n_descsz)) {
                    return;
                }
                for (uint32_t i = 0; i < hdr.hdr.n_descsz; ++i) {
                    snprintf(&dso->buildid[i * 2], 3, "%02x", buildid[i]);
                }
            }
            return;
        }
    }
}

dsoinfo_t* fetch_dso_list(mx_handle_t h, const char* name) {
    uintptr_t lmap;
    if (read_mem(h, rdebug_vaddr + rdebug_off_lmap, &lmap, sizeof(lmap))) {
        return nullptr;
    }
    dsoinfo_t* dsolist = nullptr;
    while (lmap != 0) {
        char dsoname[64];
        mx_vaddr_t base;
        uintptr_t next;
        uintptr_t str;
        if (read_mem(h, lmap + lmap_off_addr, &base, sizeof(base))) {
            break;
        }
        if (read_mem(h, lmap + lmap_off_next, &next, sizeof(next))) {
            break;
        }
        if (read_mem(h, lmap + lmap_off_name, &str, sizeof(str))) {
            break;
        }
        if (fetch_string(h, str, dsoname, sizeof(dsoname))) {
            break;
        }
        dsoinfo_t* dso = dsolist_add(&dsolist, dsoname[0] ? dsoname : name, base);
        if (dso != nullptr) {
            fetch_build_id(h, dso);
        }
        lmap = next;
    }

    return dsolist;
}

static void btprint(dsoinfo_t* list, int n, uintptr_t pc, uintptr_t sp) {
    dsoinfo_t* dso;
    for (dso = list; dso != nullptr; dso = dso->next) {
        if (pc >= dso->base) {
            break;
        }
    }
    if (dso == nullptr) {
        fprintf(stderr, "bt#%02d: pc %p sp %p\n",
                n, (void*)pc, (void*)sp);
    } else {
        fprintf(stderr, "bt#%02d: pc %p sp %p (%s,%p)\n",
                n, (void*)pc, (void*)sp, dso->name, (void*)(pc - dso->base));
    }
}

void backtrace(mx_handle_t h, uintptr_t pc, uintptr_t fp) {
    dsoinfo_t* list = fetch_dso_list(h, "app");
    int n = 1;

    for (dsoinfo_t* dso = list; dso != nullptr; dso = dso->next) {
        printf("dso: id=%s base=%p name=%s\n",
               dso->buildid, (void*)dso->base, dso->name);
    }

    // N.B. This unwinder assumes code is compiled with -fno-omit-frame-pointer
    // and -mno-omit-leaf-frame-pointer on arm64.

    btprint(list, n++, pc, fp);
    while ((fp >= 0x1000000) && (n < 50)) {
        if (read_mem(h, fp + 8, &pc, sizeof(pc))) {
            break;
        }
        btprint(list, n++, pc, fp);
        if (read_mem(h, fp, &fp, sizeof(fp))) {
            break;
        }
    }
    fprintf(stderr, "bt#%02d: end\n", n);

    while (list != nullptr) {
        dsoinfo_t* next = list->next;
        free(list);
        list = next;
    }
}
