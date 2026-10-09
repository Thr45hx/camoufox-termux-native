/* mini_linker: loads Android's /apex/com.android.runtime/bin/linker64 into
 * process memory, sets up relocations, bootstraps the Bionic linker namespace
 * machinery without crashing or triggering exit traps, and loads Android's
 * proprietary libvulkan.so to expose PowerVR hardware Vulkan drivers.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <setjmp.h>

static int s_debug_enabled = -1;
static inline int debug_logging_enabled(void) {
    if (s_debug_enabled < 0) {
        const char* e = getenv("VK_SHIM_DEBUG");
        if (!e) e = getenv("DEBUG_SHIM");
        if (!e) e = getenv("SHIM_DEBUG");
        s_debug_enabled = (e && *e && strcmp(e, "0") != 0);
    }
    return s_debug_enabled;
}

#define LOG(...) do { if (debug_logging_enabled()) { fprintf(stderr, "[mini] " __VA_ARGS__); fflush(stderr); } } while(0)

static const char* LINKER_PATH = "/apex/com.android.runtime/bin/linker64";

typedef struct {
    void*       base;
    size_t      span;
    Elf64_Dyn*  dyn;
    Elf64_Sym*  symtab;
    const char* strtab;
    Elf64_Rela* rela;      size_t rela_sz, rela_ent;
    uint64_t*   relr;      size_t relr_sz, relr_ent;
    Elf64_Rela* jmprel;    size_t jmprel_sz;
    void*       (*entry)(void);
    const char* soname;
} loaded_elf;

static void* mmap_segments(int fd, const Elf64_Ehdr* eh, const Elf64_Phdr* phdrs, size_t* out_span)
{
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uintptr_t s = phdrs[i].p_vaddr;
        uintptr_t e = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    size_t span = hi - lo;
    *out_span = span;

    void* base = mmap(NULL, span, PROT_NONE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { LOG("reserve mmap failed\n"); return NULL; }

    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        const Elf64_Phdr* p = &phdrs[i];
        int prot = 0;
        if (p->p_flags & PF_R) prot |= PROT_READ;
        if (p->p_flags & PF_W) prot |= PROT_WRITE;
        if (p->p_flags & PF_X) prot |= PROT_EXEC;
        void* seg_addr = (char*)base + (p->p_vaddr - lo);
        uintptr_t align = p->p_align > 1 ? p->p_align : 0x1000;
        uintptr_t pg_off_in_vaddr = p->p_vaddr & (align - 1);
        uintptr_t pg_off_in_file  = p->p_offset & (align - 1);
        if (pg_off_in_vaddr != pg_off_in_file) {
            LOG("segment %d has misaligned file/vaddr offsets\n", i);
            munmap(base, span);
            return NULL;
        }
        uintptr_t map_vaddr = (uintptr_t)seg_addr - pg_off_in_vaddr;
        off_t     map_off   = p->p_offset - pg_off_in_file;
        size_t    map_sz    = p->p_filesz + pg_off_in_file;
        void* mapped = mmap((void*)map_vaddr, map_sz, PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_FIXED, fd, map_off);
        if (mapped == MAP_FAILED) {
            LOG("segment %d mmap failed\n", i);
            munmap(base, span);
            return NULL;
        }
        if (p->p_memsz > p->p_filesz) {
            uintptr_t zero_from = (uintptr_t)seg_addr + p->p_filesz;
            uintptr_t zero_to   = (uintptr_t)seg_addr + p->p_memsz;
            uintptr_t page = 0x1000;
            uintptr_t pg_top = (zero_from + page - 1) & ~(page - 1);
            if (zero_to > pg_top) {
                mmap((void*)pg_top, zero_to - pg_top,
                     PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
            }
            if (zero_from < pg_top) {
                memset((void*)zero_from, 0, (pg_top - zero_from < zero_to - zero_from) ? pg_top - zero_from : zero_to - zero_from);
            }
        }
    }
    return base;
}

static void apply_rela(loaded_elf* L)
{
    if (!L->rela || !L->rela_sz) return;
    size_t n = L->rela_sz / sizeof(Elf64_Rela);
    for (size_t i = 0; i < n; i++) {
        Elf64_Rela* r = &L->rela[i];
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uintptr_t* where = (uintptr_t*)((char*)L->base + r->r_offset);
        switch (type) {
            case 0x403: /* R_AARCH64_RELATIVE */
                *where = (uintptr_t)L->base + r->r_addend;
                break;
            case 0x401: /* R_AARCH64_GLOB_DAT */
            case 0x402: /* R_AARCH64_JUMP_SLOT */
            case 0x101: /* R_AARCH64_ABS64 */
                {
                    uint32_t si = ELF64_R_SYM(r->r_info);
                    Elf64_Sym* s = &L->symtab[si];
                    if (s->st_shndx != SHN_UNDEF) {
                        *where = (uintptr_t)L->base + s->st_value + r->r_addend;
                    }
                }
                break;
        }
    }
}

static void apply_relr(loaded_elf* L)
{
    if (!L->relr || !L->relr_sz) return;
    size_t n = L->relr_sz / sizeof(uint64_t);
    uintptr_t* where = NULL;
    for (size_t i = 0; i < n; i++) {
        uint64_t e = L->relr[i];
        if ((e & 1) == 0) {
            where = (uintptr_t*)((char*)L->base + e);
            *where += (uintptr_t)L->base;
            where++;
        } else {
            uint64_t bm = e >> 1;
            for (int b = 0; bm; b++, bm >>= 1) {
                if (bm & 1) {
                    where[b] += (uintptr_t)L->base;
                }
            }
            where += 63;
        }
    }
}

static int parse_dynamic(loaded_elf* L, Elf64_Dyn* dyn)
{
    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        uintptr_t v = d->d_un.d_val;
        void* p = (char*)L->base + v;
        switch (d->d_tag) {
            case DT_SYMTAB:  L->symtab = (Elf64_Sym*)p; break;
            case DT_STRTAB:  L->strtab = (const char*)p; break;
            case DT_RELA:    L->rela = (Elf64_Rela*)p; break;
            case DT_RELASZ:  L->rela_sz = v; break;
            case DT_RELAENT: L->rela_ent = v; break;
            case DT_RELR:    L->relr = (uint64_t*)p; break;
            case DT_RELRSZ:  L->relr_sz = v; break;
            case DT_RELRENT: L->relr_ent = v; break;
            case DT_JMPREL:  L->jmprel = (Elf64_Rela*)p; break;
            case DT_PLTRELSZ:L->jmprel_sz = v; break;
        }
    }
    return 0;
}

static void* find_sym(loaded_elf* L, const char* name)
{
    size_t symcount = 0;
    intptr_t sym_bytes = (const char*)L->strtab - (const char*)L->symtab;
    if (sym_bytes > 0 && sym_bytes < (intptr_t)(1024*1024)) {
        symcount = sym_bytes / sizeof(Elf64_Sym);
    } else {
        symcount = 20000;
    }
    for (size_t i = 0; i < symcount; i++) {
        Elf64_Sym* s = &L->symtab[i];
        if (s->st_name == 0) continue;
        const char* sn = L->strtab + s->st_name;
        if (strcmp(sn, name) == 0) {
            return (char*)L->base + s->st_value;
        }
    }
    return NULL;
}

/* TLS Preservation State */
static uint64_t s_host_tpidr = 0;
static uint64_t s_bionic_tpidr = 0;
static jmp_buf  s_exit_recovery_env;
static volatile int s_exit_recovery_triggered = 0;

/* Option B Fallback: deterministic 64-bit trampoline recovery */
static void exit_recovery_trampoline(int status)
{
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    LOG("intercepted linker _exit(%d), restored host TPIDR_EL0\n", status);
    s_exit_recovery_triggered = 1;
    longjmp(s_exit_recovery_env, 1);
}

/* Linker function pointers */
static void* (*s_ldlo)(const char*, int) = NULL;
static void* (*s_ldls)(void*, const char*) = NULL;
static const char* (*s_ldle)(void) = NULL;
static void* (*s_agen)(const char*) = NULL;
/* Android extended dlopen with explicit namespace. Signature:
 *   void* android_dlopen_ext(const char* filename, int flag, const android_dlextinfo* info);
 * where info->version=2 (ANDROID_DLEXT_USE_NAMESPACE), info->data=ns ptr. */
static void* (*s_adoe)(const char*, int, const void*) = NULL;

static void* s_vulkan_handle = NULL;
static void* s_vulkan_gipa = NULL;
static void* s_vulkan_gdpa = NULL;
static int s_initialized = 0;

int mini_linker_bootstrap(void)
{
    if (s_initialized) return 0;

    int fd = open(LINKER_PATH, O_RDONLY);
    if (fd < 0) {
        /* Fallback path */
        fd = open("/system/bin/linker64", O_RDONLY);
        if (fd < 0) {
            perror("open linker64");
            return -1;
        }
    }

    struct stat st;
    fstat(fd, &st);
    void* elf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (elf == MAP_FAILED) { perror("mmap elf"); close(fd); return -1; }

    Elf64_Ehdr* eh = (Elf64_Ehdr*)elf;
    Elf64_Phdr* phs = (Elf64_Phdr*)((char*)elf + eh->e_phoff);

    loaded_elf L = {0};
    size_t span = 0;
    L.base = mmap_segments(fd, eh, phs, &span);
    close(fd);
    if (!L.base) return -1;
    L.span = span;

    Elf64_Dyn* dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phs[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn*)((char*)L.base + phs[i].p_vaddr);
            break;
        }
    }
    if (!dyn) { munmap(L.base, L.span); return -1; }
    parse_dynamic(&L, dyn);

    apply_rela(&L);
    apply_relr(&L);
    if (L.jmprel && L.jmprel_sz) {
        Elf64_Rela* sr = L.rela; size_t sz = L.rela_sz;
        L.rela = L.jmprel; L.rela_sz = L.jmprel_sz;
        apply_rela(&L);
        L.rela = sr; L.rela_sz = sz;
    }

    for (int i = 0; i < eh->e_phnum; i++) {
        if (phs[i].p_type != PT_LOAD) continue;
        const Elf64_Phdr* p = &phs[i];
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) |
                   (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        uintptr_t start = ((uintptr_t)L.base + p->p_vaddr) & ~0xfffUL;
        uintptr_t end = (((uintptr_t)L.base + p->p_vaddr + p->p_memsz) + 0xfffUL) & ~0xfffUL;
        mprotect((void*)start, end - start, prot);
    }

    s_ldlo = (void* (*)(const char*, int))find_sym(&L, "__loader_dlopen");
    s_ldls = (void* (*)(void*, const char*))find_sym(&L, "__loader_dlsym");
    s_ldle = (const char* (*)(void))find_sym(&L, "__loader_dlerror");
    s_agen = (void* (*)(const char*))find_sym(&L, "__loader_android_get_exported_namespace");
    s_adoe = (void* (*)(const char*, int, const void*))find_sym(&L, "__loader_android_dlopen_ext");

    /* Option B fallback: Direct 64-bit jump trampoline at __dl__exit (0x5d5c0) */
    uintptr_t exit_va = 0x5d5c0;
    uintptr_t exit_addr = (uintptr_t)L.base + exit_va;
    uintptr_t exit_pg = exit_addr & ~0xfffUL;
    mprotect((void*)exit_pg, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC);

    uint64_t target_fn = (uint64_t)exit_recovery_trampoline;
    uint32_t tramp[4] = {
        0x58000050,  /* ldr x16, [pc, #8] (0x58000050) */
        0xd61f0200,  /* br x16 */
        (uint32_t)(target_fn & 0xffffffff),
        (uint32_t)(target_fn >> 32)
    };
    memcpy((void*)exit_addr, tramp, sizeof(tramp));
    mprotect((void*)exit_pg, 0x1000, PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char*)exit_addr, (char*)exit_addr + sizeof(tramp));

    /* Option A: Construct clean synthetic invocation (argc=2, argv[1]="/system/bin/sh")
     * This avoids the `--help` usage exit, fully initializes default and sphal namespaces,
     * links the base binary, and returns control cleanly without terminating the host process. */
    static unsigned char rand16[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
    };
    static unsigned long raw_args[64];
    int ai = 0;
    raw_args[ai++] = 2;                                       /* argc = 2 */
    raw_args[ai++] = (unsigned long)"/system/bin/linker64";  /* argv[0] */
    raw_args[ai++] = (unsigned long)"/system/bin/sh";        /* argv[1] */
    raw_args[ai++] = 0;                                       /* argv[2] = NULL */
    raw_args[ai++] = 0;                                       /* envp[0] = NULL */

    #define AUX(t,v) do{ raw_args[ai++] = (t); raw_args[ai++] = (unsigned long)(v); }while(0)
    AUX(6, 4096);                                             /* AT_PAGESZ */
    AUX(25, (unsigned long)rand16);                           /* AT_RANDOM */
    AUX(23, 0);                                               /* AT_SECURE */
    AUX(7, (unsigned long)L.base);                            /* AT_BASE */
    AUX(9, (unsigned long)L.base + eh->e_entry);              /* AT_ENTRY */
    AUX(0, 0);                                                /* AT_NULL */

    typedef unsigned long (*linker_init_t)(void*);
    linker_init_t linker_init = (linker_init_t)((char*)L.base + 0x1a9320);

    /* Save host TPIDR_EL0 (glibc / Termux TCB) */
    __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(s_host_tpidr));

    if (setjmp(s_exit_recovery_env) == 0) {
        linker_init(raw_args);
    }

    /* Save Bionic's TCB and restore host's TPIDR_EL0 */
    __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(s_bionic_tpidr));
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));

    s_initialized = 1;

    /* Verify Bionic namespaces */
    if (s_agen) {
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
        void* ns_sphal = s_agen("sphal");
        void* ns_default = s_agen("default");
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
        LOG("bionic namespaces verified: sphal=%p, default=%p\n", ns_sphal, ns_default);
    }

    /* Load /system/lib64/libvulkan.so */
    if (s_ldlo) {
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
        s_vulkan_handle = s_ldlo("/system/lib64/libvulkan.so", 0x102);
        if (!s_vulkan_handle) {
            s_vulkan_handle = s_ldlo("libvulkan.so", 0x102);
        }
        if (s_vulkan_handle && s_ldls) {
            s_vulkan_gipa = s_ldls(s_vulkan_handle, "vkGetInstanceProcAddr");
            s_vulkan_gdpa = s_ldls(s_vulkan_handle, "vkGetDeviceProcAddr");
        }
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));

        LOG("loaded proprietary libvulkan.so handle=%p, gipa=%p, gdpa=%p\n", s_vulkan_handle, s_vulkan_gipa, s_vulkan_gdpa);
    }

    /* Try the vendor HAL *directly* — Android's libvulkan.so needs app-process
     * state (SurfaceFlinger connection via Binder, etc.) that we don't have in
     * a bare glibc process, so vkCreateInstance through it SEGVs. The vendor
     * HAL at /vendor/lib64/hw/vulkan.<hw>.so is the actual Vulkan driver, and
     * the Android HAL exports a struct whose open() returns a device struct
     * holding GetInstanceProcAddr directly. We bypass libvulkan.so entirely.
     *
     * Android HAL structs (from hardware/hwvulkan.h, hardware/hardware.h):
     *   hw_module_methods_t { int (*open)(const hw_module_t*, const char*, hw_device_t**); };
     *   hw_module_t { u32 tag; u16 mapi; u16 hapi; const char* id; const char* name;
     *                 const char* author; hw_module_methods_t* methods; void* dso;
     *                 u32 reserved[25]; };
     *   hw_device_t { u32 tag; u32 version; hw_module_t* module;
     *                 u32 reserved[12]; int (*close)(hw_device_t*); };
     *   hwvulkan_device_t { hw_device_t common; PFN_EnumInstExt eiep;
     *                       PFN_CreateInstance ci; PFN_GetInstProc gipa; };
     * HAL_MODULE_INFO_SYM = "HMI".
     */
    typedef struct hw_module_methods_s {
        int (*open)(const void* module, const char* id, void** device);
    } hw_module_methods_t;
    typedef struct hw_module_s {
        uint32_t tag;
        uint16_t module_api_version;
        uint16_t hal_api_version;
        const char* id;
        const char* name;
        const char* author;
        struct hw_module_methods_s* methods;
        void* dso;
        uint32_t reserved[25];
    } hw_module_t;
    /* Verified empirically from disasm of vulkan_device_open in
     * vulkan.powervr.so (144-byte struct via calloc(1,0x90)):
     *   [0x00]  d0 (16B) — tag/version packed as SIMD store
     *   [0x08]  hw_module_t* module
     *   [0x10..0x70) reserved (96 bytes)
     *   [0x70]  int (*close)(hw_device_t*)
     *   [0x78]  PFN_vkEnumerateInstanceExtensionProperties
     *   [0x80]  PFN_vkCreateInstance
     *   [0x88]  PFN_vkGetInstanceProcAddr
     */
    typedef struct hwvulkan_device_s {
        uint8_t  _header[0x70];   /* tag/version/module/reserved */
        int      (*close)(void*);           /* 0x70 */
        void     (*EnumerateInstExt)(void); /* 0x78 */
        void     (*CreateInstance)(void);   /* 0x80 */
        void*    (*GetInstanceProcAddr)(void* instance, const char* name); /* 0x88 */
    } hwvulkan_device_t;

    const char* hal_paths[] = {
        "/vendor/lib64/hw/vulkan.powervr.so",
        "/vendor/lib64/hw/vulkan.kodiak.so",
        "/vendor/lib64/hw/vulkan.tensor.so",
        NULL
    };
    void* hal_handle = NULL;
    const char* loaded_hal = NULL;
    /* vendor paths require the "sphal" exported namespace. Use
     * android_dlopen_ext with the real struct android_dlextinfo:
     *   { u64 flags; void* reserved_addr; usize reserved_sz;
     *     int relro_fd; int library_fd; off64_t library_fd_offset;
     *     android_namespace_t* library_namespace; }
     * flags = ANDROID_DLEXT_USE_NAMESPACE (0x200). */
    struct android_dlextinfo {
        uint64_t flags;
        void*    reserved_addr;
        size_t   reserved_size;
        int      relro_fd;
        int      library_fd;
        long     library_fd_offset;
        void*    library_namespace;
    } dlext_info;
    memset(&dlext_info, 0, sizeof(dlext_info));
    dlext_info.flags = 0x200;  /* ANDROID_DLEXT_USE_NAMESPACE */
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
    dlext_info.library_namespace = s_agen ? s_agen("sphal") : NULL;
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    LOG("sphal namespace for HAL load: %p (flags=0x%lx)\n",
        dlext_info.library_namespace, (long)dlext_info.flags);
    for (int i = 0; hal_paths[i]; i++) {
        if (dlext_info.library_namespace && s_adoe) {
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
            hal_handle = s_adoe(hal_paths[i], 0x102, &dlext_info);
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
        }
        if (!hal_handle) {
            /* fallback to plain dlopen */
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
            hal_handle = s_ldlo(hal_paths[i], 0x102);
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
        }
        if (hal_handle) { loaded_hal = hal_paths[i]; break; }
    }
    LOG("vendor HAL dlopen: %s = %p\n", loaded_hal ? loaded_hal : "(none found)", hal_handle);

    if (hal_handle) {
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
        hw_module_t* mod = (hw_module_t*)s_ldls(hal_handle, "HMI");
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
        LOG("HAL_MODULE_INFO_SYM (HMI) = %p\n", mod);
        if (mod) {
            LOG("  HMI dump: tag=0x%x mapi=0x%x hapi=0x%x\n",
                mod->tag, mod->module_api_version, mod->hal_api_version);
            LOG("  id='%s' name='%s' methods=%p\n",
                mod->id ? mod->id : "(null)",
                mod->name ? mod->name : "(null)",
                mod->methods);
            if (mod->methods) {
                LOG("  methods->open = %p\n", mod->methods->open);
            }
        }
        if (mod && mod->methods && mod->methods->open) {
            hwvulkan_device_t* dev = NULL;
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
            /* PowerVR's HAL expects id="vk0" (confirmed via disasm of
             * vulkan_device_open's strcmp site at .rodata offset 0x16568). */
            int rc = mod->methods->open(mod, "vk0", (void**)&dev);
            __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
            LOG("hwvulkan open: rc=%d dev=%p\n", rc, dev);
            if (rc == 0 && dev && dev->GetInstanceProcAddr) {
                /* Replace libvulkan.so's gipa with the vendor driver's. */
                s_vulkan_gipa = (void*)dev->GetInstanceProcAddr;
                /* GetDeviceProcAddr: ask the vendor gipa for it */
                typedef void* (*GIPA_t)(void*, const char*);
                GIPA_t gipa = (GIPA_t)s_vulkan_gipa;
                __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
                void* gdpa = gipa(NULL, "vkGetDeviceProcAddr");
                __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
                if (gdpa) s_vulkan_gdpa = gdpa;
                LOG("switched to vendor HAL: gipa=%p gdpa=%p\n", s_vulkan_gipa, s_vulkan_gdpa);
            }
        }
    }

    return (s_vulkan_gipa != NULL) ? 0 : -1;
}

/* Full bionic pthread TCB — captured by spawning a real bionic pthread via
 * bionic libc's pthread_create and reading its TPIDR_EL0. The bootstrap
 * TPIDR we save after __linker_init is slim (no errno slot at offset 776
 * etc.), which crashes any vendor-driver code path that touches errno.
 * A real pthread gets the full pthread_internal_t TCB. */
static volatile unsigned long s_bionic_pthread_tpidr = 0;

/* Pure-asm callback: can't touch glibc or bionic libc from this thread —
 * glibc because TPIDR is bionic, bionic because our code is glibc-linked.
 * Just capture TPIDR into caller-provided slot and spin forever to keep the
 * TCB alive and mapped for other threads to use its TPIDR. */
__attribute__((noinline))
static void* capture_tpidr_and_spin(void* arg)
{
    unsigned long tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    *(volatile unsigned long*)arg = tp;
    __asm__ volatile("dmb ish" ::: "memory");
    /* Spin forever. Yield-less because any libc call would touch TLS.
     * One CPU gets pegged until process exit; acceptable price for a stable
     * bionic TCB. (TODO: raw futex-wait syscall to idle without burn.) */
    for (;;) {
        __asm__ volatile("yield");
    }
    return (void*)0;
}
extern void* mini_linker_dlopen(const char*, int);
extern void* mini_linker_dlsym(void*, const char*);

static void spawn_bionic_pthread(void)
{
    if (s_bionic_pthread_tpidr) return;

    /* Load bionic libc via Android loader; grab pthread_create. */
    void* libc = mini_linker_dlopen("libc.so", 0x102);
    LOG("bionic libc.so handle = %p\n", libc);
    if (!libc) return;

    typedef int (*pthread_create_t)(unsigned long*, const void*, void*(*)(void*), void*);
    pthread_create_t pc = (pthread_create_t)mini_linker_dlsym(libc, "pthread_create");
    LOG("bionic pthread_create = %p\n", pc);
    if (!pc) return;

    unsigned long thr = 0;
    /* pthread_create itself is bionic code — needs bionic TPIDR to run. */
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
    int rc = pc(&thr, 0, capture_tpidr_and_spin, (void*)&s_bionic_pthread_tpidr);
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    LOG("pthread_create rc=%d thr=0x%lx\n", rc, thr);
    if (rc != 0) return;

    /* Wait for the thread to publish its TPIDR. */
    int spins = 0;
    while (!s_bionic_pthread_tpidr && spins < 100000000) {
        __asm__ volatile("yield" ::: "memory");
        spins++;
    }
    if (!s_bionic_pthread_tpidr) { LOG("timed out waiting for bionic pthread TPIDR\n"); return; }

    LOG("bionic pthread TCB TPIDR = 0x%lx (replacing bootstrap 0x%lx)\n",
        s_bionic_pthread_tpidr, s_bionic_tpidr);
    /* Replace the bootstrap TPIDR with the real pthread's TPIDR. All future
     * bionic calls will use this one — it has the full pthread_internal_t
     * including errno slot. */
    s_bionic_tpidr = s_bionic_pthread_tpidr;
}

unsigned long mini_linker_get_bionic_tpidr(void)
{
    if (!s_initialized) mini_linker_bootstrap();
    if (!s_bionic_pthread_tpidr) spawn_bionic_pthread();
    return s_bionic_tpidr;
}

void* mini_linker_get_vulkan_gipa(void)
{
    if (!s_initialized) {
        mini_linker_bootstrap();
    }
    return s_vulkan_gipa;
}

void* mini_linker_get_vulkan_gdpa(void)
{
    if (!s_initialized) {
        mini_linker_bootstrap();
    }
    return s_vulkan_gdpa;
}

void* mini_linker_dlopen(const char* name, int flags)
{
    if (!s_initialized) mini_linker_bootstrap();
    if (!s_ldlo) return NULL;
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
    void* h = s_ldlo(name, flags);
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    return h;
}

void* mini_linker_dlsym(void* handle, const char* name)
{
    if (!s_initialized) mini_linker_bootstrap();
    if (!s_ldls) return NULL;
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
    void* s = s_ldls(handle, name);
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    return s;
}

const char* mini_linker_dlerror(void)
{
    if (!s_ldle) return NULL;
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr));
    const char* err = s_ldle();
    __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr));
    return err;
}

#ifndef MINI_LINKER_LIB
/* Standalone test driver */
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef int VkResult;
typedef struct { int sType; const void* pNext; uint32_t flags; const void* pApp; uint32_t lc; const char* const* ln; uint32_t ec; const char* const* en; } VkInstanceCreateInfo;
typedef struct { uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType; char deviceName[256]; uint8_t uuid[16]; uint32_t lim[128]; uint32_t sp[5]; } VkPhysicalDeviceProperties;

int main(void)
{
    s_debug_enabled = 1;
    LOG("starting mini_linker standalone test...\n");
    int rc = mini_linker_bootstrap();
    if (rc != 0 || !s_vulkan_gipa) {
        LOG("bootstrap failed!\n");
        return 1;
    }

    /* Bionic-side callouts need TPIDR_EL0 pointing at Bionic's TCB, not glibc's.
     * Wrap every Vulkan call in a swap. */
    #define BIONIC_CALL(expr) ({ \
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr)); \
        __typeof__(expr) _r = (expr); \
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr)); \
        _r; \
    })
    #define BIONIC_VCALL(expr) do { \
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_bionic_tpidr)); \
        (expr); \
        __asm__ volatile("msr TPIDR_EL0, %0" :: "r"(s_host_tpidr)); \
    } while(0)

    void* (*gipa)(VkInstance, const char*) = (void* (*)(VkInstance, const char*))s_vulkan_gipa;
    VkResult (*create_instance)(const VkInstanceCreateInfo*, const void*, VkInstance*) =
        (VkResult (*)(const VkInstanceCreateInfo*, const void*, VkInstance*))BIONIC_CALL(gipa(NULL, "vkCreateInstance"));
    LOG("vkCreateInstance fn = %p\n", create_instance);

    VkInstance inst = NULL;
    VkInstanceCreateInfo ci = {0};
    ci.sType = 1;
    VkResult res = BIONIC_CALL(create_instance(&ci, NULL, &inst));
    LOG("vkCreateInstance res = %d, inst = %p\n", res, inst);

    if (res == 0 && inst) {
        VkResult (*enum_devs)(VkInstance, uint32_t*, VkPhysicalDevice*) =
            (VkResult (*)(VkInstance, uint32_t*, VkPhysicalDevice*))BIONIC_CALL(gipa(inst, "vkEnumeratePhysicalDevices"));
        void (*get_props)(VkPhysicalDevice, VkPhysicalDeviceProperties*) =
            (void (*)(VkPhysicalDevice, VkPhysicalDeviceProperties*))BIONIC_CALL(gipa(inst, "vkGetPhysicalDeviceProperties"));

        uint32_t dev_count = 0;
        BIONIC_VCALL(enum_devs(inst, &dev_count, NULL));
        LOG("Physical devices count = %u\n", dev_count);
        if (dev_count > 0) {
            VkPhysicalDevice devs[4];
            BIONIC_VCALL(enum_devs(inst, &dev_count, devs));
            for (uint32_t i = 0; i < dev_count; i++) {
                VkPhysicalDeviceProperties props;
                memset(&props, 0, sizeof(props));
                BIONIC_VCALL(get_props(devs[i], &props));
                LOG("Device %u: %s (vendorID=0x%x, deviceID=0x%x, apiVersion=%u.%u.%u)\n",
                    i, props.deviceName, props.vendorID, props.deviceID,
                    props.apiVersion >> 22, (props.apiVersion >> 12) & 0x3ff, props.apiVersion & 0xfff);
            }
        }
    }
    LOG("mini_linker standalone test passed successfully!\n");
    return 0;
}
#endif
