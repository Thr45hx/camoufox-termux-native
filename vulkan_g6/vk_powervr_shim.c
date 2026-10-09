#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

extern void* mini_linker_get_vulkan_gipa(void);
extern unsigned long mini_linker_get_bionic_tpidr(void);  /* exposed below */

typedef void *VkInstance;
typedef void *VkDevice;
typedef void *VkPhysicalDevice;
typedef void *VkQueue;
typedef void *VkCommandBuffer;
typedef int VkResult;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkGetInstanceProcAddr android_gipa = (PFN_vkGetInstanceProcAddr)0;
static VkInstance s_last_instance = (VkInstance)0;

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

static unsigned long s_host_tpidr = 0;

static void raw_puts(const char* s)
{
    size_t len = 0;
    while (s[len]) len++;
    syscall(SYS_write, 2, s, len);
}

static void raw_hex(unsigned long v)
{
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int nib = (v >> (i * 4)) & 0xf;
        buf[17 - i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[18] = '\0';
    raw_puts(buf);
}

static void crash_handler(int sig, siginfo_t* info, void* uctx)
{
    if (s_host_tpidr) {
        __asm__ volatile("msr tpidr_el0, %0" :: "r"(s_host_tpidr));
    }
    ucontext_t* uc = (ucontext_t*)uctx;
    unsigned long pc = uc->uc_mcontext.pc;
    unsigned long sp = uc->uc_mcontext.sp;
    raw_puts("\n=== SHIM CAUGHT CRASH ===\nsig: ");
    raw_hex(sig);
    raw_puts("\nfault addr: ");
    raw_hex((unsigned long)info->si_addr);
    raw_puts("\npc: ");
    raw_hex(pc);
    raw_puts("\nsp: ");
    raw_hex(sp);
    raw_puts("\nregisters:\n");
    for (int r = 0; r < 31; r++) {
        raw_puts(" x");
        char rn[3];
        if (r < 10) { rn[0] = ' '; rn[1] = '0' + r; }
        else { rn[0] = '0' + (r/10); rn[1] = '0' + (r%10); }
        rn[2] = '\0';
        raw_puts(rn);
        raw_puts(": ");
        raw_hex(uc->uc_mcontext.regs[r]);
        if ((r + 1) % 2 == 0) raw_puts("\n");
    }
    raw_puts("\n=========================\n");
    syscall(SYS_exit_group, 128 + sig);
}

__attribute__((constructor))
static void install_crash_handler(void)
{
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(s_host_tpidr));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static void load_android_vulkan(void)
{
    if (android_gipa) return;
    android_gipa = (PFN_vkGetInstanceProcAddr)mini_linker_get_vulkan_gipa();
}

/* Call android_gipa with TPIDR_EL0 swapped to the bionic TCB. */
static void* safe_gipa(VkInstance inst, const char* name)
{
    unsigned long bionic_tp = mini_linker_get_bionic_tpidr();
    unsigned long host_tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(host_tp));
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(bionic_tp));
    void* r = (void*)android_gipa(inst, name);
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(host_tp));
    return r;
}

/* Call any bionic function with TPIDR_EL0 swapped. Macro for brevity. */
#define BIONIC_CALL(expr) ({ \
    unsigned long _btp = mini_linker_get_bionic_tpidr(); \
    unsigned long _htp; \
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(_htp)); \
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(_btp)); \
    __typeof__(expr) _r = (expr); \
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(_htp)); \
    _r; \
})

#define BIONIC_VCALL(expr) do { \
    unsigned long _btp = mini_linker_get_bionic_tpidr(); \
    unsigned long _htp; \
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(_htp)); \
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(_btp)); \
    (expr); \
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(_htp)); \
} while(0)

/* Explicit C wrappers for the entry points Khronos loader may dlsym
 * directly off our .so (bypassing GIPA + trampoline). Each wrapper swaps
 * TPIDR_EL0 to bionic, calls the vendor driver, swaps back. */

typedef int (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, void*);
typedef int (*PFN_vkEnumerateInstanceLayerProperties)(uint32_t*, void*);
typedef int (*PFN_vkEnumerateInstanceVersion)(uint32_t*);

__attribute__((visibility("default")))
int vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count, void* props)
{
    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) { if (count) *count = 0; return 0; }
    PFN_vkEnumerateInstanceExtensionProperties fn =
        (PFN_vkEnumerateInstanceExtensionProperties)safe_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!fn) { if (count) *count = 0; return 0; }
    return BIONIC_CALL(fn(layer, count, props));
}

__attribute__((visibility("default")))
int vkEnumerateInstanceLayerProperties(uint32_t* count, void* props)
{
    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) { if (count) *count = 0; return 0; }
    PFN_vkEnumerateInstanceLayerProperties fn =
        (PFN_vkEnumerateInstanceLayerProperties)safe_gipa(NULL, "vkEnumerateInstanceLayerProperties");
    if (!fn) { if (count) *count = 0; return 0; }
    return BIONIC_CALL(fn(count, props));
}

__attribute__((visibility("default")))
int vkEnumerateInstanceVersion(uint32_t* pApiVersion)
{
    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) { if (pApiVersion) *pApiVersion = 0x400139; return 0; }  /* 1.4.317 */
    PFN_vkEnumerateInstanceVersion fn =
        (PFN_vkEnumerateInstanceVersion)safe_gipa(NULL, "vkEnumerateInstanceVersion");
    if (!fn) { if (pApiVersion) *pApiVersion = 0x400139; return 0; }
    return BIONIC_CALL(fn(pApiVersion));
}

#define WRAPPED_MAGIC 0x57524150U  /* 'WRAP' */

static int s_shim_debug = -1;
static inline int shim_debug_enabled(void)
{
    if (s_shim_debug < 0) {
        const char* e = getenv("VK_SHIM_DEBUG");
        if (!e) e = getenv("DEBUG_SHIM");
        if (!e) e = getenv("SHIM_DEBUG");
        s_shim_debug = (e && *e && strcmp(e, "0") != 0);
    }
    return s_shim_debug;
}
#define SHIM_LOG(...) do { if (shim_debug_enabled()) { fprintf(stderr, "[shim] " __VA_ARGS__); fflush(stderr); } } while(0)

/* WrappedInstance / WrappedPhysicalDevice: same shape as WrappedDevice.
 * Khronos loader at interface version 1+ overwrites offset 0 of every
 * dispatchable handle (instance, phys dev, queue, device, cmd buffer) with
 * its own dispatch magic. Android bionic driver uses offset 0 for ITS own
 * dispatch. We allocate a wrapper {loader_dispatch@0, real@8, magic@16}, hand
 * the wrapper to the loader (so loader's clobber of offset 0 is harmless), and
 * every trampoline into the vendor driver checks magic at offset 16 before doing
 * `ldr x0, [x0, #8]` to unwrap before the real call. Non-wrapped handles like
 * VkQueue and VkCommandBuffer pass through untouched. */
struct WrappedInstance { void* loader_dispatch; void* real_instance; uint32_t magic; uint32_t pad; };
struct WrappedPhysicalDevice { void* loader_dispatch; void* real_physdev; uint32_t magic; uint32_t pad; };

static inline VkInstance unwrap_instance(VkInstance inst)
{
    if (!inst) return s_last_instance;
    if (inst == s_last_instance) return inst;
    struct WrappedInstance* w = (struct WrappedInstance*)inst;
    if (w->magic == WRAPPED_MAGIC && w->real_instance) return w->real_instance;
    return inst;
}

static struct {
    VkPhysicalDevice real;
    struct WrappedPhysicalDevice* wrapped;
} s_phys_map[16];
static uint32_t s_phys_map_count = 0;

static inline VkPhysicalDevice wrap_physdev(VkPhysicalDevice real)
{
    for (uint32_t i = 0; i < s_phys_map_count; i++) {
        if (s_phys_map[i].real == real) {
            return (VkPhysicalDevice)s_phys_map[i].wrapped;
        }
    }
    return real;
}

static inline VkPhysicalDevice unwrap_physdev(VkPhysicalDevice pd)
{
    if (!pd) return NULL;
    for (uint32_t i = 0; i < s_phys_map_count; i++) {
        if (pd == (VkPhysicalDevice)s_phys_map[i].wrapped) {
            return s_phys_map[i].real;
        }
        if (pd == s_phys_map[i].real) {
            return pd;
        }
    }
    struct WrappedPhysicalDevice* w = (struct WrappedPhysicalDevice*)pd;
    if (w->magic == WRAPPED_MAGIC && w->real_physdev) return (VkPhysicalDevice)w->real_physdev;
    return pd;
}

/* Wrapper for vkCreateInstance. Called from Khronos loader (host TPIDR). */
static VkResult my_vkCreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance)
{
    if (!android_gipa) load_android_vulkan();
    typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, VkInstance*);
    PFN_vkCreateInstance real_create = (PFN_vkCreateInstance)safe_gipa(NULL, "vkCreateInstance");
    if (!real_create) return -1;
    VkInstance real_inst = NULL;
    VkResult res = BIONIC_CALL(real_create(pCreateInfo, pAllocator, &real_inst));
    if (res == 0 && real_inst) {
        s_last_instance = real_inst;
        struct WrappedInstance* w = (struct WrappedInstance*)calloc(1, sizeof(*w));
        w->real_instance = real_inst;
        w->magic = WRAPPED_MAGIC;
        *pInstance = (VkInstance)w;
    }
    return res;
}

/* Wrapper for vkEnumeratePhysicalDevices — wrap each returned handle. */
static VkResult my_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pPhysDevs)
{
    VkInstance real_inst = unwrap_instance(instance);
    typedef VkResult (*PFN_t)(VkInstance, uint32_t*, VkPhysicalDevice*);
    PFN_t real = (PFN_t)safe_gipa(real_inst, "vkEnumeratePhysicalDevices");
    if (!real) return -1;

    if (!pPhysDevs) {
        /* First pass: just report count. */
        return BIONIC_CALL(real(real_inst, pCount, NULL));
    }
    /* Second pass: fill, then wrap each. */
    VkResult res = BIONIC_CALL(real(real_inst, pCount, pPhysDevs));
    if (res != 0) return res;
    s_phys_map_count = 0;
    for (uint32_t i = 0; i < *pCount; i++) {
        struct WrappedPhysicalDevice* w = (struct WrappedPhysicalDevice*)calloc(1, sizeof(*w));
        w->real_physdev = pPhysDevs[i];
        w->magic = WRAPPED_MAGIC;
        if (i < 16) {
            s_phys_map[i].real = pPhysDevs[i];
            s_phys_map[i].wrapped = w;
            s_phys_map_count++;
        }
        pPhysDevs[i] = (VkPhysicalDevice)w;
    }
    return res;
}

typedef struct {
    int sType;
    void* pNext;
    uint32_t physicalDeviceCount;
    VkPhysicalDevice physicalDevices[32];
    uint32_t subsetAllocation;
} VkPhysicalDeviceGroupPropertiesInternal;

static VkResult my_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pCount, void* pProperties)
{
    VkInstance real_inst = unwrap_instance(instance);
    typedef VkResult (*PFN_t)(VkInstance, uint32_t*, void*);
    PFN_t real = (PFN_t)safe_gipa(real_inst, "vkEnumeratePhysicalDeviceGroups");
    if (!real) {
        real = (PFN_t)safe_gipa(real_inst, "vkEnumeratePhysicalDeviceGroupsKHR");
    }
    if (!real) return -1;

    if (!pProperties) {
        return BIONIC_CALL(real(real_inst, pCount, NULL));
    }
    VkResult res = BIONIC_CALL(real(real_inst, pCount, pProperties));
    if (res != 0) return res;

    VkPhysicalDeviceGroupPropertiesInternal* props = (VkPhysicalDeviceGroupPropertiesInternal*)pProperties;
    for (uint32_t g = 0; g < *pCount; g++) {
        for (uint32_t d = 0; d < props[g].physicalDeviceCount; d++) {
            props[g].physicalDevices[d] = wrap_physdev(props[g].physicalDevices[d]);
        }
    }
    return res;
}

__attribute__((visibility("default")))
VkResult vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pCount, void* pProperties)
{
    return my_vkEnumeratePhysicalDeviceGroups(instance, pCount, pProperties);
}

__attribute__((visibility("default")))
VkResult vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* pCount, void* pProperties)
{
    return my_vkEnumeratePhysicalDeviceGroups(instance, pCount, pProperties);
}

static VkResult my_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, void* pProperties)
{
    typedef VkResult (*PFN_t)(VkPhysicalDevice, const char*, uint32_t*, void*);
    PFN_t real = (PFN_t)safe_gipa(s_last_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return -1;
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    return BIONIC_CALL(real(real_pd, pLayerName, pPropertyCount, pProperties));
}

static VkResult my_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, void* pProperties)
{
    if (pPropertyCount) *pPropertyCount = 0;
    return 0;
}

struct VkBaseOutStructure {
    int sType;
    struct VkBaseOutStructure* pNext;
};

typedef void (*PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, void*);

static void my_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, void* pProperties)
{
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    PFN_vkGetPhysicalDeviceProperties2 real = (PFN_vkGetPhysicalDeviceProperties2)safe_gipa(s_last_instance, "vkGetPhysicalDeviceProperties2");
    if (!real) {
        real = (PFN_vkGetPhysicalDeviceProperties2)safe_gipa(s_last_instance, "vkGetPhysicalDeviceProperties2KHR");
    }
    if (real && real_pd) {
        BIONIC_VCALL(real(real_pd, pProperties));
    }
    if (pProperties) {
        struct VkBaseOutStructure* cur = (struct VkBaseOutStructure*)pProperties;
        while (cur) {
            if (cur->sType == 1000353000) { /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT */
                typedef struct {
                    int sType;
                    void* pNext;
                    uint32_t hasPrimary;
                    uint32_t hasRender;
                    int64_t primaryMajor;
                    int64_t primaryMinor;
                    int64_t renderMajor;
                    int64_t renderMinor;
                } VkDrmProps;
                VkDrmProps* drm = (VkDrmProps*)cur;
                drm->hasPrimary = 1;
                drm->primaryMajor = 226;
                drm->primaryMinor = 0;
                drm->hasRender = 1;
                drm->renderMajor = 226;
                drm->renderMinor = 128;
                SHIM_LOG("Intercepted VkPhysicalDeviceDrmPropertiesEXT: spoofed primary (226, 0) and render (226, 128)\n");
            }
            cur = cur->pNext;
        }
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, void* pProperties)
{
    my_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, void* pProperties)
{
    my_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}

typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void*);

static void my_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, void* pProperties)
{
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    PFN_vkGetPhysicalDeviceProperties real = (PFN_vkGetPhysicalDeviceProperties)safe_gipa(s_last_instance, "vkGetPhysicalDeviceProperties");
    if (real && real_pd) {
        BIONIC_VCALL(real(real_pd, pProperties));
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, void* pProperties)
{
    my_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
}

__attribute__((visibility("default")))
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, void* pProperties)
{
    return my_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

__attribute__((visibility("default")))
VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, void* pProperties)
{
    return my_vkEnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
}

/* Also handle vkGetDeviceQueue → wraps VkQueue; etc. For now just these two.
 * Queue/CommandBuffer wrapping can follow the same pattern when apps need
 * them, but minimal Vulkan info enumeration doesn't require it. */

/* Exported alias so Khronos loader can dlsym it directly. */
__attribute__((visibility("default")))
VkResult vkCreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance)
{
    return my_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

__attribute__((visibility("default")))
uint32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    /* With WrappedInstance + WrappedPhysicalDevice + WrappedDevice all in
     * place, we can support interface 5 and let the loader use its full
     * dispatch chain. */
    uint32_t want = *pSupportedVersion;
    if (want > 5) want = 5;
    *pSupportedVersion = want;
    return 0;  /* VK_SUCCESS */
}

/*
 * WrappedDevice: Khronos loader expects loader_dispatch at offset 0.
 * Android's libvulkan.so expects its own dispatch table at offset 0 of real_dev.
 * By returning WrappedDevice to Khronos loader, Khronos loader safely writes its
 * dispatch table to loader_dispatch without touching real_dev's dispatch table.
 */
struct WrappedDevice {
    void* loader_dispatch;  /* Offset 0: used by Khronos loader */
    void* real_device;      /* Offset 8: Android's real VkDevice */
    uint32_t magic;         /* Offset 16: WRAPPED_MAGIC */
    uint32_t pad;
};

static struct WrappedDevice* s_wrapped_dev = NULL;

static inline VkDevice unwrap_device(VkDevice dev)
{
    if (!dev) return dev;
    struct WrappedDevice* w = (struct WrappedDevice*)dev;
    if (w->magic == WRAPPED_MAGIC && w->real_device) {
        return (VkDevice)w->real_device;
    }
    return dev;
}

struct WrappedQueue {
    void* loader_dispatch;  /* Offset 0: used by Khronos loader */
    void* real_queue;       /* Offset 8: Android's real VkQueue */
    uint32_t magic;         /* Offset 16: WRAPPED_MAGIC */
    uint32_t pad;
};

static struct WrappedQueue s_wrapped_queues[16];
static uint32_t s_wrapped_queue_count = 0;

static inline VkQueue wrap_queue(VkQueue real_q)
{
    if (!real_q) return real_q;
    for (uint32_t i = 0; i < s_wrapped_queue_count; i++) {
        if (s_wrapped_queues[i].real_queue == real_q) {
            return (VkQueue)&s_wrapped_queues[i];
        }
    }
    if (s_wrapped_queue_count < 16) {
        uint32_t idx = s_wrapped_queue_count++;
        s_wrapped_queues[idx].loader_dispatch = NULL;
        s_wrapped_queues[idx].real_queue = real_q;
        s_wrapped_queues[idx].magic = WRAPPED_MAGIC;
        return (VkQueue)&s_wrapped_queues[idx];
    }
    return real_q;
}

static inline VkQueue unwrap_queue(VkQueue q)
{
    if (!q) return q;
    struct WrappedQueue* w = (struct WrappedQueue*)q;
    if (w->magic == WRAPPED_MAGIC && w->real_queue) {
        return (VkQueue)w->real_queue;
    }
    return q;
}

struct WrappedCommandBuffer {
    void* loader_dispatch;  /* Offset 0: used by Khronos loader */
    void* real_cmd_buffer;  /* Offset 8: Android's real VkCommandBuffer */
    uint32_t magic;         /* Offset 16: WRAPPED_MAGIC */
    uint32_t pad;
};

static inline VkCommandBuffer wrap_cmd_buffer(VkCommandBuffer real_cb)
{
    if (!real_cb) return real_cb;
    struct WrappedCommandBuffer* w = (struct WrappedCommandBuffer*)calloc(1, sizeof(*w));
    w->real_cmd_buffer = real_cb;
    w->magic = WRAPPED_MAGIC;
    return (VkCommandBuffer)w;
}

static inline VkCommandBuffer unwrap_cmd_buffer(VkCommandBuffer cb)
{
    if (!cb) return cb;
    struct WrappedCommandBuffer* w = (struct WrappedCommandBuffer*)cb;
    if (w->magic == WRAPPED_MAGIC && w->real_cmd_buffer) {
        return (VkCommandBuffer)w->real_cmd_buffer;
    }
    return cb;
}

static VkResult my_vkCreateDevice(VkPhysicalDevice physicalDevice, const void* pCreateInfo, const void* pAllocator, VkDevice* pDevice)
{
    SHIM_LOG("my_vkCreateDevice ENTER: pd=%p, pCI=%p, pDev=%p\n", physicalDevice, pCreateInfo, pDevice);
    if (!android_gipa) load_android_vulkan();
    typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void*, const void*, VkDevice*);
    PFN_vkCreateDevice real_cd = (PFN_vkCreateDevice)safe_gipa(s_last_instance, "vkCreateDevice");
    SHIM_LOG("my_vkCreateDevice: real_cd=%p, last_inst=%p\n", real_cd, s_last_instance);
    /* Unwrap physical device safely. */
    if (physicalDevice)
        physicalDevice = unwrap_physdev(physicalDevice);
    SHIM_LOG("my_vkCreateDevice: unwrapped_pd=%p\n", physicalDevice);

    /* Sanitise pNext chain for Bionic */
    typedef struct {
        int sType; const void* pNext; uint32_t flags;
        uint32_t queueCreateInfoCount; const void* pQueueCreateInfos;
        uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
        uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
        const void* pEnabledFeatures;
    } VkDeviceCreateInfoInternal;

    const VkDeviceCreateInfoInternal* dci = (const VkDeviceCreateInfoInternal*)pCreateInfo;
    SHIM_LOG("my_vkCreateDevice: qCount=%u, extCount=%u\n", dci->queueCreateInfoCount, dci->enabledExtensionCount);
    for (uint32_t i = 0; i < dci->enabledExtensionCount; i++) {
        SHIM_LOG("  ext[%u]: %s\n", i, dci->ppEnabledExtensionNames[i]);
    }
    VkDeviceCreateInfoInternal dci_copy = *dci;
    dci_copy.pNext = NULL;
    dci_copy.pEnabledFeatures = NULL;

    VkDevice real_dev = NULL;
    SHIM_LOG("my_vkCreateDevice: calling real_cd...\n");
    VkResult res = BIONIC_CALL(real_cd(physicalDevice, &dci_copy, pAllocator, &real_dev));
    SHIM_LOG("my_vkCreateDevice: real_cd returned res=%d, real_dev=%p\n", res, real_dev);
    if (res != 0 || !real_dev) return res;

    s_wrapped_dev = (struct WrappedDevice*)calloc(1, sizeof(struct WrappedDevice));
    s_wrapped_dev->real_device = real_dev;
    s_wrapped_dev->magic = WRAPPED_MAGIC;
    *pDevice = (VkDevice)s_wrapped_dev;
    SHIM_LOG("my_vkCreateDevice: returning wrapped_dev=%p\n", s_wrapped_dev);
    return 0;
}

__attribute__((visibility("default")))
VkResult vkCreateDevice(VkPhysicalDevice physicalDevice, const void* pCreateInfo, const void* pAllocator, VkDevice* pDevice)
{
    return my_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

static void my_vkDestroyInstance(VkInstance instance, const void* pAllocator)
{
    typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);
    PFN_vkDestroyInstance real_di = (PFN_vkDestroyInstance)safe_gipa(s_last_instance, "vkDestroyInstance");
    VkInstance real_inst = instance;
    if (instance) {
        real_inst = unwrap_instance(instance);
        free(instance);
    }
    if (s_last_instance == real_inst) s_last_instance = NULL;
    if (real_di && real_inst) {
        BIONIC_VCALL(real_di(real_inst, pAllocator));
    }
}

static void my_vkDestroyDevice(VkDevice dev, const void* pAllocator)
{
    typedef void (*PFN_vkDestroyDevice)(VkDevice, const void*);
    PFN_vkDestroyDevice real_dd = (PFN_vkDestroyDevice)safe_gipa(s_last_instance, "vkDestroyDevice");
    if (s_wrapped_dev && dev == (VkDevice)s_wrapped_dev) {
        if (real_dd) BIONIC_VCALL(real_dd(s_wrapped_dev->real_device, pAllocator));
        free(s_wrapped_dev);
        s_wrapped_dev = NULL;
    } else {
        if (real_dd) BIONIC_VCALL(real_dd(dev, pAllocator));
    }
}

static void my_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
{
    SHIM_LOG("my_vkGetDeviceQueue ENTER: dev=%p, qfi=%u, qi=%u, pQueue=%p\n", device, queueFamilyIndex, queueIndex, pQueue);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
    PFN_vkGetDeviceQueue real_fn = (PFN_vkGetDeviceQueue)safe_gipa(s_last_instance, "vkGetDeviceQueue");
    if (!real_fn) {
        SHIM_LOG("my_vkGetDeviceQueue: real_fn NULL!\n");
        return;
    }
    VkQueue real_q = NULL;
    BIONIC_VCALL(real_fn(real_dev, queueFamilyIndex, queueIndex, &real_q));
    SHIM_LOG("my_vkGetDeviceQueue: real_fn returned real_q=%p\n", real_q);
    if (pQueue) {
        *pQueue = wrap_queue(real_q);
        SHIM_LOG("my_vkGetDeviceQueue: wrapped queue = %p\n", *pQueue);
    }
}

static void my_vkGetDeviceQueue2(VkDevice device, const void* pQueueInfo, VkQueue* pQueue)
{
    SHIM_LOG("my_vkGetDeviceQueue2 ENTER: dev=%p, pInfo=%p, pQueue=%p\n", device, pQueueInfo, pQueue);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_vkGetDeviceQueue2)(VkDevice, const void*, VkQueue*);
    PFN_vkGetDeviceQueue2 real_fn = (PFN_vkGetDeviceQueue2)safe_gipa(s_last_instance, "vkGetDeviceQueue2");
    if (!real_fn) real_fn = (PFN_vkGetDeviceQueue2)safe_gipa(s_last_instance, "vkGetDeviceQueue2KHR");
    if (!real_fn) {
        SHIM_LOG("my_vkGetDeviceQueue2: real_fn NULL!\n");
        return;
    }
    VkQueue real_q = NULL;
    BIONIC_VCALL(real_fn(real_dev, pQueueInfo, &real_q));
    SHIM_LOG("my_vkGetDeviceQueue2: real_fn returned real_q=%p\n", real_q);
    if (pQueue) {
        *pQueue = wrap_queue(real_q);
        SHIM_LOG("my_vkGetDeviceQueue2: wrapped queue = %p\n", *pQueue);
    }
}

__attribute__((visibility("default")))
void vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
{
    my_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

__attribute__((visibility("default")))
void vkGetDeviceQueue2(VkDevice device, const void* pQueueInfo, VkQueue* pQueue)
{
    my_vkGetDeviceQueue2(device, pQueueInfo, pQueue);
}

static void my_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, int format, void* pFormatProperties)
{
    SHIM_LOG("my_vkGetPhysicalDeviceFormatProperties ENTER: pd=%p, format=%d (0x%x), pProps=%p\n",
             physicalDevice, format, (unsigned)format, pFormatProperties);
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    typedef void (*PFN_t)(VkPhysicalDevice, int, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceFormatProperties");
    if (real_fn && real_pd && pFormatProperties) {
        BIONIC_VCALL(real_fn(real_pd, format, pFormatProperties));
    }
    SHIM_LOG("my_vkGetPhysicalDeviceFormatProperties EXIT\n");
}

static void my_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, int format, void* pFormatProperties)
{
    SHIM_LOG("my_vkGetPhysicalDeviceFormatProperties2 ENTER: pd=%p, format=%d (0x%x), pProps=%p\n",
             physicalDevice, format, (unsigned)format, pFormatProperties);
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    typedef void (*PFN_t)(VkPhysicalDevice, int, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceFormatProperties2");
    if (!real_fn) real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceFormatProperties2KHR");
    if (real_fn && real_pd && pFormatProperties) {
        BIONIC_VCALL(real_fn(real_pd, format, pFormatProperties));
    }
    SHIM_LOG("my_vkGetPhysicalDeviceFormatProperties2 EXIT\n");
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, int format, void* pFormatProperties)
{
    my_vkGetPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, int format, void* pFormatProperties)
{
    my_vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physicalDevice, int format, void* pFormatProperties)
{
    my_vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}

static VkResult my_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const void* pImageFormatInfo, void* pImageFormatProperties)
{
    SHIM_LOG("my_vkGetPhysicalDeviceImageFormatProperties2 ENTER: pd=%p, pInfo=%p, pProps=%p\n",
             physicalDevice, pImageFormatInfo, pImageFormatProperties);
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    typedef VkResult (*PFN_t)(VkPhysicalDevice, const void*, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!real_fn) real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    VkResult res = -1;
    if (real_fn && real_pd && pImageFormatProperties) {
        res = BIONIC_CALL(real_fn(real_pd, pImageFormatInfo, pImageFormatProperties));
    }
    SHIM_LOG("my_vkGetPhysicalDeviceImageFormatProperties2 EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const void* pImageFormatInfo, void* pImageFormatProperties)
{
    return my_vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

__attribute__((visibility("default")))
VkResult vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice physicalDevice, const void* pImageFormatInfo, void* pImageFormatProperties)
{
    return my_vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

static void my_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, void* pFeatures)
{
    SHIM_LOG("my_vkGetPhysicalDeviceFeatures2 ENTER: pd=%p, pFeatures=%p\n", physicalDevice, pFeatures);
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    typedef void (*PFN_t)(VkPhysicalDevice, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceFeatures2");
    if (!real_fn) real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (real_fn && real_pd && pFeatures) {
        BIONIC_VCALL(real_fn(real_pd, pFeatures));
    }
    SHIM_LOG("my_vkGetPhysicalDeviceFeatures2 EXIT\n");
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, void* pFeatures)
{
    my_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, void* pFeatures)
{
    my_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

static void my_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, void* pMemoryProperties)
{
    SHIM_LOG("my_vkGetPhysicalDeviceMemoryProperties ENTER: pd=%p, pProps=%p\n", physicalDevice, pMemoryProperties);
    VkPhysicalDevice real_pd = unwrap_physdev(physicalDevice);
    typedef void (*PFN_t)(VkPhysicalDevice, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (real_fn && real_pd && pMemoryProperties) {
        BIONIC_VCALL(real_fn(real_pd, pMemoryProperties));
    }
    SHIM_LOG("my_vkGetPhysicalDeviceMemoryProperties EXIT\n");
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, void* pMemoryProperties)
{
    my_vkGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
}

static VkResult my_vkCreateSamplerYcbcrConversion(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pYcbcrConversion)
{
    SHIM_LOG("my_vkCreateSamplerYcbcrConversion ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pYcbcrConversion);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateSamplerYcbcrConversion");
    if (!real_fn) real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateSamplerYcbcrConversionKHR");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pYcbcrConversion));
    }
    SHIM_LOG("my_vkCreateSamplerYcbcrConversion EXIT: res=%d, out=%p\n", res, pYcbcrConversion ? *pYcbcrConversion : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateSamplerYcbcrConversion(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pYcbcrConversion)
{
    return my_vkCreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
}

__attribute__((visibility("default")))
VkResult vkCreateSamplerYcbcrConversionKHR(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pYcbcrConversion)
{
    return my_vkCreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
}

static VkResult my_vkCreateSampler(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSampler)
{
    SHIM_LOG("my_vkCreateSampler ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pSampler);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateSampler");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pSampler));
    }
    SHIM_LOG("my_vkCreateSampler EXIT: res=%d, out=%p\n", res, pSampler ? *pSampler : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateSampler(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSampler)
{
    return my_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
}

static VkResult my_vkCreateDescriptorSetLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSetLayout)
{
    SHIM_LOG("my_vkCreateDescriptorSetLayout ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pSetLayout);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateDescriptorSetLayout");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pSetLayout));
    }
    SHIM_LOG("my_vkCreateDescriptorSetLayout EXIT: res=%d, out=%p\n", res, pSetLayout ? *pSetLayout : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateDescriptorSetLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSetLayout)
{
    return my_vkCreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
}

static VkResult my_vkCreatePipelineLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pPipelineLayout)
{
    SHIM_LOG("my_vkCreatePipelineLayout ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pPipelineLayout);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreatePipelineLayout");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pPipelineLayout));
    }
    SHIM_LOG("my_vkCreatePipelineLayout EXIT: res=%d, out=%p\n", res, pPipelineLayout ? *pPipelineLayout : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreatePipelineLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pPipelineLayout)
{
    return my_vkCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
}

static VkResult my_vkCreateCommandPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pCommandPool)
{
    SHIM_LOG("my_vkCreateCommandPool ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pCommandPool);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateCommandPool");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pCommandPool));
    }
    SHIM_LOG("my_vkCreateCommandPool EXIT: res=%d, out=%p\n", res, pCommandPool ? *pCommandPool : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateCommandPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pCommandPool)
{
    return my_vkCreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
}

static VkResult my_vkCreateDescriptorPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pDescriptorPool)
{
    SHIM_LOG("my_vkCreateDescriptorPool ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pDescriptorPool);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateDescriptorPool");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pDescriptorPool));
    }
    SHIM_LOG("my_vkCreateDescriptorPool EXIT: res=%d, out=%p\n", res, pDescriptorPool ? *pDescriptorPool : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateDescriptorPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pDescriptorPool)
{
    return my_vkCreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
}

static VkResult my_vkCreateShaderModule(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pShaderModule)
{
    SHIM_LOG("my_vkCreateShaderModule ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pShaderModule);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateShaderModule");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pShaderModule));
    }
    SHIM_LOG("my_vkCreateShaderModule EXIT: res=%d, out=%p\n", res, pShaderModule ? *pShaderModule : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateShaderModule(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pShaderModule)
{
    return my_vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
}

static VkResult my_vkAllocateDescriptorSets(VkDevice device, const void* pAllocateInfo, void** pDescriptorSets)
{
    SHIM_LOG("my_vkAllocateDescriptorSets ENTER: dev=%p, pAI=%p, pOut=%p\n", device, pAllocateInfo, pDescriptorSets);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkAllocateDescriptorSets");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pAllocateInfo, pDescriptorSets));
    }
    SHIM_LOG("my_vkAllocateDescriptorSets EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkAllocateDescriptorSets(VkDevice device, const void* pAllocateInfo, void** pDescriptorSets)
{
    return my_vkAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
}

static VkResult my_vkCreateBuffer(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pBuffer)
{
    SHIM_LOG("my_vkCreateBuffer ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pBuffer);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateBuffer");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pBuffer));
    }
    SHIM_LOG("my_vkCreateBuffer EXIT: res=%d, out=%p\n", res, pBuffer ? *pBuffer : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateBuffer(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pBuffer)
{
    return my_vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
}

static void my_vkGetBufferMemoryRequirements(VkDevice device, void* buffer, void* pMemoryRequirements)
{
    SHIM_LOG("my_vkGetBufferMemoryRequirements ENTER: dev=%p, buf=%p, pReqs=%p\n", device, buffer, pMemoryRequirements);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetBufferMemoryRequirements");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, buffer, pMemoryRequirements));
    }
    SHIM_LOG("my_vkGetBufferMemoryRequirements EXIT\n");
}

__attribute__((visibility("default")))
void vkGetBufferMemoryRequirements(VkDevice device, void* buffer, void* pMemoryRequirements)
{
    my_vkGetBufferMemoryRequirements(device, buffer, pMemoryRequirements);
}

static VkResult my_vkAllocateMemory(VkDevice device, const void* pAllocateInfo, const void* pAllocator, void** pMemory)
{
    SHIM_LOG("my_vkAllocateMemory ENTER: dev=%p, pAI=%p, pOut=%p\n", device, pAllocateInfo, pMemory);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkAllocateMemory");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pAllocateInfo, pAllocator, pMemory));
    }
    SHIM_LOG("my_vkAllocateMemory EXIT: res=%d, out=%p\n", res, pMemory ? *pMemory : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkAllocateMemory(VkDevice device, const void* pAllocateInfo, const void* pAllocator, void** pMemory)
{
    return my_vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
}

static VkResult my_vkBindBufferMemory(VkDevice device, void* buffer, void* memory, uint64_t memoryOffset)
{
    SHIM_LOG("my_vkBindBufferMemory ENTER: dev=%p, buf=%p, mem=%p, off=%lu\n", device, buffer, memory, (unsigned long)memoryOffset);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, void*, void*, uint64_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkBindBufferMemory");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, buffer, memory, memoryOffset));
    }
    SHIM_LOG("my_vkBindBufferMemory EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkBindBufferMemory(VkDevice device, void* buffer, void* memory, uint64_t memoryOffset)
{
    return my_vkBindBufferMemory(device, buffer, memory, memoryOffset);
}

static VkResult my_vkMapMemory(VkDevice device, void* memory, uint64_t offset, uint64_t size, uint32_t flags, void** ppData)
{
    SHIM_LOG("my_vkMapMemory ENTER: dev=%p, mem=%p, off=%lu, sz=%lu, flags=%u\n", device, memory, (unsigned long)offset, (unsigned long)size, flags);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, void*, uint64_t, uint64_t, uint32_t, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkMapMemory");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, memory, offset, size, flags, ppData));
    }
    SHIM_LOG("my_vkMapMemory EXIT: res=%d, ppData=%p\n", res, ppData ? *ppData : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkMapMemory(VkDevice device, void* memory, uint64_t offset, uint64_t size, uint32_t flags, void** ppData)
{
    return my_vkMapMemory(device, memory, offset, size, flags, ppData);
}

static VkResult my_vkCreateImage(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pImage)
{
    SHIM_LOG("my_vkCreateImage ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pImage);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateImage");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pImage));
    }
    SHIM_LOG("my_vkCreateImage EXIT: res=%d, out=%p\n", res, pImage ? *pImage : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateImage(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pImage)
{
    return my_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
}

static void my_vkGetImageMemoryRequirements(VkDevice device, void* image, void* pMemoryRequirements)
{
    SHIM_LOG("my_vkGetImageMemoryRequirements ENTER: dev=%p, img=%p, pReqs=%p\n", device, image, pMemoryRequirements);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetImageMemoryRequirements");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, image, pMemoryRequirements));
    }
    SHIM_LOG("my_vkGetImageMemoryRequirements EXIT\n");
}

__attribute__((visibility("default")))
void vkGetImageMemoryRequirements(VkDevice device, void* image, void* pMemoryRequirements)
{
    my_vkGetImageMemoryRequirements(device, image, pMemoryRequirements);
}

static VkResult my_vkBindImageMemory(VkDevice device, void* image, void* memory, uint64_t memoryOffset)
{
    SHIM_LOG("my_vkBindImageMemory ENTER: dev=%p, img=%p, mem=%p, off=%lu\n", device, image, memory, (unsigned long)memoryOffset);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, void*, void*, uint64_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkBindImageMemory");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, image, memory, memoryOffset));
    }
    SHIM_LOG("my_vkBindImageMemory EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkBindImageMemory(VkDevice device, void* image, void* memory, uint64_t memoryOffset)
{
    return my_vkBindImageMemory(device, image, memory, memoryOffset);
}

static VkResult my_vkCreateImageView(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pView)
{
    SHIM_LOG("my_vkCreateImageView ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pView);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateImageView");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pView));
    }
    SHIM_LOG("my_vkCreateImageView EXIT: res=%d, out=%p\n", res, pView ? *pView : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateImageView(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pView)
{
    return my_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
}

typedef struct VkCommandBufferAllocateInfoInternal {
    int sType;
    const void* pNext;
    void* commandPool;
    int level;
    uint32_t commandBufferCount;
} VkCommandBufferAllocateInfoInternal;

static VkResult my_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfoInternal* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{
    SHIM_LOG("my_vkAllocateCommandBuffers ENTER: dev=%p, pAI=%p, count=%u\n",
             device, pAllocateInfo, pAllocateInfo ? pAllocateInfo->commandBufferCount : 0);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const VkCommandBufferAllocateInfoInternal*, VkCommandBuffer*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkAllocateCommandBuffers");
    if (!real_fn || !real_dev || !pAllocateInfo || !pCommandBuffers) return -1;

    uint32_t count = pAllocateInfo->commandBufferCount;
    VkCommandBuffer* temp = (VkCommandBuffer*)calloc(count, sizeof(VkCommandBuffer));
    VkResult res = BIONIC_CALL(real_fn(real_dev, pAllocateInfo, temp));
    SHIM_LOG("my_vkAllocateCommandBuffers: real_fn returned %d\n", res);
    if (res == 0) {
        for (uint32_t i = 0; i < count; i++) {
            pCommandBuffers[i] = wrap_cmd_buffer(temp[i]);
            SHIM_LOG("  cb[%u]: real=%p -> wrapped=%p\n", i, temp[i], pCommandBuffers[i]);
        }
    }
    free(temp);
    SHIM_LOG("my_vkAllocateCommandBuffers EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfoInternal* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{
    return my_vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
}

static void my_vkFreeCommandBuffers(VkDevice device, void* commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{
    SHIM_LOG("my_vkFreeCommandBuffers ENTER: dev=%p, count=%u\n", device, commandBufferCount);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, uint32_t, const VkCommandBuffer*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkFreeCommandBuffers");
    if (real_fn && real_dev && pCommandBuffers) {
        VkCommandBuffer* temp = (VkCommandBuffer*)calloc(commandBufferCount, sizeof(VkCommandBuffer));
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            temp[i] = unwrap_cmd_buffer(pCommandBuffers[i]);
        }
        BIONIC_VCALL(real_fn(real_dev, commandPool, commandBufferCount, temp));
        free(temp);
    }
    SHIM_LOG("my_vkFreeCommandBuffers EXIT\n");
}

__attribute__((visibility("default")))
void vkFreeCommandBuffers(VkDevice device, void* commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{
    my_vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

typedef struct VkSubmitInfoInternal {
    int sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores;
    const void* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const void* pSignalSemaphores;
} VkSubmitInfoInternal;

static VkResult my_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfoInternal* pSubmits, void* fence)
{
    SHIM_LOG("my_vkQueueSubmit ENTER: queue=%p, submitCount=%u, fence=%p\n", queue, submitCount, fence);
    VkQueue real_q = unwrap_queue(queue);
    typedef VkResult (*PFN_t)(VkQueue, uint32_t, const VkSubmitInfoInternal*, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkQueueSubmit");
    if (!real_fn || !real_q) {
        SHIM_LOG("my_vkQueueSubmit: real_fn (%p) or real_q (%p) NULL!\n", real_fn, real_q);
        return -1;
    }

    if (!pSubmits || submitCount == 0) {
        VkResult res = BIONIC_CALL(real_fn(real_q, submitCount, pSubmits, fence));
        SHIM_LOG("my_vkQueueSubmit EXIT: res=%d\n", res);
        return res;
    }

    VkSubmitInfoInternal* copy = (VkSubmitInfoInternal*)calloc(submitCount, sizeof(VkSubmitInfoInternal));
    VkCommandBuffer** cb_arrays = (VkCommandBuffer**)calloc(submitCount, sizeof(VkCommandBuffer*));

    for (uint32_t i = 0; i < submitCount; i++) {
        copy[i] = pSubmits[i];
        if (pSubmits[i].commandBufferCount > 0 && pSubmits[i].pCommandBuffers) {
            uint32_t cb_count = pSubmits[i].commandBufferCount;
            cb_arrays[i] = (VkCommandBuffer*)calloc(cb_count, sizeof(VkCommandBuffer));
            for (uint32_t j = 0; j < cb_count; j++) {
                cb_arrays[i][j] = unwrap_cmd_buffer(pSubmits[i].pCommandBuffers[j]);
                SHIM_LOG("  submit[%u] cb[%u]: %p -> real %p\n", i, j, pSubmits[i].pCommandBuffers[j], cb_arrays[i][j]);
            }
            copy[i].pCommandBuffers = cb_arrays[i];
        }
    }

    VkResult res = BIONIC_CALL(real_fn(real_q, submitCount, copy, fence));
    SHIM_LOG("my_vkQueueSubmit EXIT: res=%d\n", res);

    for (uint32_t i = 0; i < submitCount; i++) {
        if (cb_arrays[i]) free(cb_arrays[i]);
    }
    free(cb_arrays);
    free(copy);

    return res;
}

__attribute__((visibility("default")))
VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfoInternal* pSubmits, void* fence)
{
    return my_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

static VkResult my_vkQueueWaitIdle(VkQueue queue)
{
    SHIM_LOG("my_vkQueueWaitIdle ENTER: queue=%p\n", queue);
    VkQueue real_q = unwrap_queue(queue);
    typedef VkResult (*PFN_t)(VkQueue);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkQueueWaitIdle");
    VkResult res = -1;
    if (real_fn && real_q) {
        res = BIONIC_CALL(real_fn(real_q));
    }
    SHIM_LOG("my_vkQueueWaitIdle EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkQueueWaitIdle(VkQueue queue)
{
    return my_vkQueueWaitIdle(queue);
}

static VkResult my_vkDeviceWaitIdle(VkDevice device)
{
    SHIM_LOG("my_vkDeviceWaitIdle ENTER: dev=%p\n", device);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDeviceWaitIdle");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev));
    }
    SHIM_LOG("my_vkDeviceWaitIdle EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkDeviceWaitIdle(VkDevice device)
{
    return my_vkDeviceWaitIdle(device);
}

static VkResult my_vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const void* pBeginInfo)
{
    SHIM_LOG("my_vkBeginCommandBuffer ENTER: cb=%p, pBI=%p\n", commandBuffer, pBeginInfo);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    SHIM_LOG("my_vkBeginCommandBuffer: real_cb=%p\n", real_cb);
    typedef VkResult (*PFN_t)(VkCommandBuffer, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkBeginCommandBuffer");
    SHIM_LOG("my_vkBeginCommandBuffer: real_fn=%p\n", real_fn);
    VkResult res = -1;
    if (real_fn && real_cb) {
        res = BIONIC_CALL(real_fn(real_cb, pBeginInfo));
    }
    SHIM_LOG("my_vkBeginCommandBuffer EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const void* pBeginInfo)
{
    return my_vkBeginCommandBuffer(commandBuffer, pBeginInfo);
}

static VkResult my_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    SHIM_LOG("my_vkEndCommandBuffer ENTER: cb=%p\n", commandBuffer);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef VkResult (*PFN_t)(VkCommandBuffer);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkEndCommandBuffer");
    VkResult res = -1;
    if (real_fn && real_cb) {
        res = BIONIC_CALL(real_fn(real_cb));
    }
    SHIM_LOG("my_vkEndCommandBuffer EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    return my_vkEndCommandBuffer(commandBuffer);
}

static VkResult my_vkResetCommandBuffer(VkCommandBuffer commandBuffer, uint32_t flags)
{
    SHIM_LOG("my_vkResetCommandBuffer ENTER: cb=%p, flags=%u\n", commandBuffer, flags);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef VkResult (*PFN_t)(VkCommandBuffer, uint32_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkResetCommandBuffer");
    VkResult res = -1;
    if (real_fn && real_cb) {
        res = BIONIC_CALL(real_fn(real_cb, flags));
    }
    SHIM_LOG("my_vkResetCommandBuffer EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkResetCommandBuffer(VkCommandBuffer commandBuffer, uint32_t flags)
{
    return my_vkResetCommandBuffer(commandBuffer, flags);
}

static void my_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer,
                                    uint32_t srcStageMask,
                                    uint32_t dstStageMask,
                                    uint32_t dependencyFlags,
                                    uint32_t memoryBarrierCount,
                                    const void* pMemoryBarriers,
                                    uint32_t bufferMemoryBarrierCount,
                                    const void* pBufferMemoryBarriers,
                                    uint32_t imageMemoryBarrierCount,
                                    const void* pImageMemoryBarriers)
{
    SHIM_LOG("my_vkCmdPipelineBarrier ENTER: cb=%p, imgBarriers=%u\n", commandBuffer, imageMemoryBarrierCount);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, uint32_t, uint32_t, uint32_t,
                          uint32_t, const void*,
                          uint32_t, const void*,
                          uint32_t, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdPipelineBarrier");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, srcStageMask, dstStageMask, dependencyFlags,
                             memoryBarrierCount, pMemoryBarriers,
                             bufferMemoryBarrierCount, pBufferMemoryBarriers,
                             imageMemoryBarrierCount, pImageMemoryBarriers));
    }
    SHIM_LOG("my_vkCmdPipelineBarrier EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdPipelineBarrier(VkCommandBuffer commandBuffer,
                          uint32_t srcStageMask,
                          uint32_t dstStageMask,
                          uint32_t dependencyFlags,
                          uint32_t memoryBarrierCount,
                          const void* pMemoryBarriers,
                          uint32_t bufferMemoryBarrierCount,
                          const void* pBufferMemoryBarriers,
                          uint32_t imageMemoryBarrierCount,
                          const void* pImageMemoryBarriers)
{
    my_vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                            memoryBarrierCount, pMemoryBarriers,
                            bufferMemoryBarrierCount, pBufferMemoryBarriers,
                            imageMemoryBarrierCount, pImageMemoryBarriers);
}

static void my_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                                      void* srcBuffer,
                                      void* dstImage,
                                      uint32_t dstImageLayout,
                                      uint32_t regionCount,
                                      const void* pRegions)
{
    SHIM_LOG("my_vkCmdCopyBufferToImage ENTER: cb=%p, srcBuf=%p, dstImg=%p, regions=%u\n",
             commandBuffer, srcBuffer, dstImage, regionCount);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, void*, void*, uint32_t, uint32_t, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdCopyBufferToImage");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions));
    }
    SHIM_LOG("my_vkCmdCopyBufferToImage EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                            void* srcBuffer,
                            void* dstImage,
                            uint32_t dstImageLayout,
                            uint32_t regionCount,
                            const void* pRegions)
{
    my_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

static void my_vkCmdCopyImage(VkCommandBuffer commandBuffer,
                              void* srcImage,
                              uint32_t srcImageLayout,
                              void* dstImage,
                              uint32_t dstImageLayout,
                              uint32_t regionCount,
                              const void* pRegions)
{
    SHIM_LOG("my_vkCmdCopyImage ENTER: cb=%p, srcImg=%p, dstImg=%p, regions=%u\n",
             commandBuffer, srcImage, dstImage, regionCount);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, void*, uint32_t, void*, uint32_t, uint32_t, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdCopyImage");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions));
    }
    SHIM_LOG("my_vkCmdCopyImage EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdCopyImage(VkCommandBuffer commandBuffer,
                    void* srcImage,
                    uint32_t srcImageLayout,
                    void* dstImage,
                    uint32_t dstImageLayout,
                    uint32_t regionCount,
                    const void* pRegions)
{
    my_vkCmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

static void my_vkCmdBindPipeline(VkCommandBuffer commandBuffer,
                                 uint32_t pipelineBindPoint,
                                 void* pipeline)
{
    SHIM_LOG("my_vkCmdBindPipeline ENTER: cb=%p, bp=%u, pipe=%p\n", commandBuffer, pipelineBindPoint, pipeline);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, uint32_t, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdBindPipeline");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, pipelineBindPoint, pipeline));
    }
    SHIM_LOG("my_vkCmdBindPipeline EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdBindPipeline(VkCommandBuffer commandBuffer,
                       uint32_t pipelineBindPoint,
                       void* pipeline)
{
    my_vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

static void my_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                       uint32_t pipelineBindPoint,
                                       void* layout,
                                       uint32_t firstSet,
                                       uint32_t descriptorSetCount,
                                       const void** pDescriptorSets,
                                       uint32_t dynamicOffsetCount,
                                       const uint32_t* pDynamicOffsets)
{
    SHIM_LOG("my_vkCmdBindDescriptorSets ENTER: cb=%p, sets=%u\n", commandBuffer, descriptorSetCount);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, uint32_t, void*, uint32_t, uint32_t, const void**, uint32_t, const uint32_t*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdBindDescriptorSets");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets));
    }
    SHIM_LOG("my_vkCmdBindDescriptorSets EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                             uint32_t pipelineBindPoint,
                             void* layout,
                             uint32_t firstSet,
                             uint32_t descriptorSetCount,
                             const void** pDescriptorSets,
                             uint32_t dynamicOffsetCount,
                             const uint32_t* pDynamicOffsets)
{
    my_vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

static void my_vkCmdDispatch(VkCommandBuffer commandBuffer,
                             uint32_t groupCountX,
                             uint32_t groupCountY,
                             uint32_t groupCountZ)
{
    SHIM_LOG("my_vkCmdDispatch ENTER: cb=%p, %u,%u,%u\n", commandBuffer, groupCountX, groupCountY, groupCountZ);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdDispatch");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, groupCountX, groupCountY, groupCountZ));
    }
    SHIM_LOG("my_vkCmdDispatch EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdDispatch(VkCommandBuffer commandBuffer,
                   uint32_t groupCountX,
                   uint32_t groupCountY,
                   uint32_t groupCountZ)
{
    my_vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

static void my_vkCmdResetQueryPool(VkCommandBuffer commandBuffer,
                                   void* queryPool,
                                   uint32_t firstQuery,
                                   uint32_t queryCount)
{
    SHIM_LOG("my_vkCmdResetQueryPool ENTER: cb=%p, qp=%p, first=%u, count=%u\n", commandBuffer, queryPool, firstQuery, queryCount);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, void*, uint32_t, uint32_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdResetQueryPool");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, queryPool, firstQuery, queryCount));
    }
    SHIM_LOG("my_vkCmdResetQueryPool EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdResetQueryPool(VkCommandBuffer commandBuffer,
                         void* queryPool,
                         uint32_t firstQuery,
                         uint32_t queryCount)
{
    my_vkCmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);
}

static void my_vkCmdWriteTimestamp(VkCommandBuffer commandBuffer,
                                   uint32_t pipelineStage,
                                   void* queryPool,
                                   uint32_t query)
{
    SHIM_LOG("my_vkCmdWriteTimestamp ENTER: cb=%p, stage=%u, qp=%p, q=%u\n", commandBuffer, pipelineStage, queryPool, query);
    VkCommandBuffer real_cb = unwrap_cmd_buffer(commandBuffer);
    typedef void (*PFN_t)(VkCommandBuffer, uint32_t, void*, uint32_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCmdWriteTimestamp");
    if (real_fn && real_cb) {
        BIONIC_VCALL(real_fn(real_cb, pipelineStage, queryPool, query));
    }
    SHIM_LOG("my_vkCmdWriteTimestamp EXIT\n");
}

__attribute__((visibility("default")))
void vkCmdWriteTimestamp(VkCommandBuffer commandBuffer,
                         uint32_t pipelineStage,
                         void* queryPool,
                         uint32_t query)
{
    my_vkCmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
}

static VkResult my_vkCreateComputePipelines(VkDevice device, void* pipelineCache, uint32_t createInfoCount, const void* pCreateInfos, const void* pAllocator, void** pPipelines)
{
    SHIM_LOG("my_vkCreateComputePipelines ENTER: dev=%p, count=%u\n", device, createInfoCount);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, void*, uint32_t, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateComputePipelines");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines));
    }
    SHIM_LOG("my_vkCreateComputePipelines EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateComputePipelines(VkDevice device, void* pipelineCache, uint32_t createInfoCount, const void* pCreateInfos, const void* pAllocator, void** pPipelines)
{
    return my_vkCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

static void my_vkDestroyPipeline(VkDevice device, void* pipeline, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyPipeline ENTER: dev=%p, pipe=%p\n", device, pipeline);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyPipeline");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, pipeline, pAllocator));
    }
    SHIM_LOG("my_vkDestroyPipeline EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyPipeline(VkDevice device, void* pipeline, const void* pAllocator)
{
    my_vkDestroyPipeline(device, pipeline, pAllocator);
}

static void my_vkDestroyImage(VkDevice device, void* image, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyImage ENTER: dev=%p, img=%p\n", device, image);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyImage");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, image, pAllocator));
    }
    SHIM_LOG("my_vkDestroyImage EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyImage(VkDevice device, void* image, const void* pAllocator)
{
    my_vkDestroyImage(device, image, pAllocator);
}

static void my_vkDestroyImageView(VkDevice device, void* imageView, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyImageView ENTER: dev=%p, iv=%p\n", device, imageView);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyImageView");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, imageView, pAllocator));
    }
    SHIM_LOG("my_vkDestroyImageView EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyImageView(VkDevice device, void* imageView, const void* pAllocator)
{
    my_vkDestroyImageView(device, imageView, pAllocator);
}

static void my_vkDestroyBuffer(VkDevice device, void* buffer, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyBuffer ENTER: dev=%p, buf=%p\n", device, buffer);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyBuffer");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, buffer, pAllocator));
    }
    SHIM_LOG("my_vkDestroyBuffer EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyBuffer(VkDevice device, void* buffer, const void* pAllocator)
{
    my_vkDestroyBuffer(device, buffer, pAllocator);
}

static void my_vkFreeMemory(VkDevice device, void* memory, const void* pAllocator)
{
    SHIM_LOG("my_vkFreeMemory ENTER: dev=%p, mem=%p\n", device, memory);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkFreeMemory");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, memory, pAllocator));
    }
    SHIM_LOG("my_vkFreeMemory EXIT\n");
}

__attribute__((visibility("default")))
void vkFreeMemory(VkDevice device, void* memory, const void* pAllocator)
{
    my_vkFreeMemory(device, memory, pAllocator);
}

static void my_vkUnmapMemory(VkDevice device, void* memory)
{
    SHIM_LOG("my_vkUnmapMemory ENTER: dev=%p, mem=%p\n", device, memory);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkUnmapMemory");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, memory));
    }
    SHIM_LOG("my_vkUnmapMemory EXIT\n");
}

__attribute__((visibility("default")))
void vkUnmapMemory(VkDevice device, void* memory)
{
    my_vkUnmapMemory(device, memory);
}

static void my_vkDestroyCommandPool(VkDevice device, void* commandPool, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyCommandPool ENTER: dev=%p, cp=%p\n", device, commandPool);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyCommandPool");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, commandPool, pAllocator));
    }
    SHIM_LOG("my_vkDestroyCommandPool EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyCommandPool(VkDevice device, void* commandPool, const void* pAllocator)
{
    my_vkDestroyCommandPool(device, commandPool, pAllocator);
}

static VkResult my_vkCreateSemaphore(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSemaphore)
{
    SHIM_LOG("my_vkCreateSemaphore ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pSemaphore);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateSemaphore");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pSemaphore));
    }
    SHIM_LOG("my_vkCreateSemaphore EXIT: res=%d, out=%p\n", res, pSemaphore ? *pSemaphore : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateSemaphore(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pSemaphore)
{
    return my_vkCreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
}

static void my_vkDestroySemaphore(VkDevice device, void* semaphore, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroySemaphore ENTER: dev=%p, sem=%p\n", device, semaphore);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroySemaphore");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, semaphore, pAllocator));
    }
    SHIM_LOG("my_vkDestroySemaphore EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroySemaphore(VkDevice device, void* semaphore, const void* pAllocator)
{
    my_vkDestroySemaphore(device, semaphore, pAllocator);
}

static VkResult my_vkCreateFence(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pFence)
{
    SHIM_LOG("my_vkCreateFence ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pFence);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateFence");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pFence));
    }
    SHIM_LOG("my_vkCreateFence EXIT: res=%d, out=%p\n", res, pFence ? *pFence : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateFence(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pFence)
{
    return my_vkCreateFence(device, pCreateInfo, pAllocator, pFence);
}

static void my_vkDestroyFence(VkDevice device, void* fence, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyFence ENTER: dev=%p, fence=%p\n", device, fence);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyFence");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, fence, pAllocator));
    }
    SHIM_LOG("my_vkDestroyFence EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyFence(VkDevice device, void* fence, const void* pAllocator)
{
    my_vkDestroyFence(device, fence, pAllocator);
}

static VkResult my_vkResetFences(VkDevice device, uint32_t fenceCount, const void** pFences)
{
    SHIM_LOG("my_vkResetFences ENTER: dev=%p, count=%u\n", device, fenceCount);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, uint32_t, const void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkResetFences");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, fenceCount, pFences));
    }
    SHIM_LOG("my_vkResetFences EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkResetFences(VkDevice device, uint32_t fenceCount, const void** pFences)
{
    return my_vkResetFences(device, fenceCount, pFences);
}

static VkResult my_vkWaitForFences(VkDevice device, uint32_t fenceCount, const void** pFences, uint32_t waitAll, uint64_t timeout)
{
    SHIM_LOG("my_vkWaitForFences ENTER: dev=%p, count=%u\n", device, fenceCount);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, uint32_t, const void**, uint32_t, uint64_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkWaitForFences");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, fenceCount, pFences, waitAll, timeout));
    }
    SHIM_LOG("my_vkWaitForFences EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const void** pFences, uint32_t waitAll, uint64_t timeout)
{
    return my_vkWaitForFences(device, fenceCount, pFences, waitAll, timeout);
}

static VkResult my_vkCreateQueryPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pQueryPool)
{
    SHIM_LOG("my_vkCreateQueryPool ENTER: dev=%p, pCI=%p, pOut=%p\n", device, pCreateInfo, pQueryPool);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, const void*, const void*, void**);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkCreateQueryPool");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, pCreateInfo, pAllocator, pQueryPool));
    }
    SHIM_LOG("my_vkCreateQueryPool EXIT: res=%d, out=%p\n", res, pQueryPool ? *pQueryPool : NULL);
    return res;
}

__attribute__((visibility("default")))
VkResult vkCreateQueryPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, void** pQueryPool)
{
    return my_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
}

static void my_vkDestroyQueryPool(VkDevice device, void* queryPool, const void* pAllocator)
{
    SHIM_LOG("my_vkDestroyQueryPool ENTER: dev=%p, qp=%p\n", device, queryPool);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkDestroyQueryPool");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, queryPool, pAllocator));
    }
    SHIM_LOG("my_vkDestroyQueryPool EXIT\n");
}

__attribute__((visibility("default")))
void vkDestroyQueryPool(VkDevice device, void* queryPool, const void* pAllocator)
{
    my_vkDestroyQueryPool(device, queryPool, pAllocator);
}

static VkResult my_vkGetQueryPoolResults(VkDevice device, void* queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, uint64_t stride, uint32_t flags)
{
    SHIM_LOG("my_vkGetQueryPoolResults ENTER: dev=%p, qp=%p, first=%u, count=%u\n", device, queryPool, firstQuery, queryCount);
    VkDevice real_dev = unwrap_device(device);
    typedef VkResult (*PFN_t)(VkDevice, void*, uint32_t, uint32_t, size_t, void*, uint64_t, uint32_t);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetQueryPoolResults");
    VkResult res = -1;
    if (real_fn && real_dev) {
        res = BIONIC_CALL(real_fn(real_dev, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags));
    }
    SHIM_LOG("my_vkGetQueryPoolResults EXIT: res=%d\n", res);
    return res;
}

__attribute__((visibility("default")))
VkResult vkGetQueryPoolResults(VkDevice device, void* queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, uint64_t stride, uint32_t flags)
{
    return my_vkGetQueryPoolResults(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
}

static void my_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const void* pDescriptorWrites, uint32_t descriptorCopyCount, const void* pDescriptorCopies)
{
    SHIM_LOG("my_vkUpdateDescriptorSets ENTER: dev=%p, writes=%u\n", device, descriptorWriteCount);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, uint32_t, const void*, uint32_t, const void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkUpdateDescriptorSets");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies));
    }
    SHIM_LOG("my_vkUpdateDescriptorSets EXIT\n");
}

__attribute__((visibility("default")))
void vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const void* pDescriptorWrites, uint32_t descriptorCopyCount, const void* pDescriptorCopies)
{
    my_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

static void my_vkGetImageSubresourceLayout(VkDevice device, void* image, const void* pSubresource, void* pLayout)
{
    SHIM_LOG("my_vkGetImageSubresourceLayout ENTER: dev=%p, img=%p\n", device, image);
    VkDevice real_dev = unwrap_device(device);
    typedef void (*PFN_t)(VkDevice, void*, const void*, void*);
    PFN_t real_fn = (PFN_t)safe_gipa(s_last_instance, "vkGetImageSubresourceLayout");
    if (real_fn && real_dev) {
        BIONIC_VCALL(real_fn(real_dev, image, pSubresource, pLayout));
    }
    SHIM_LOG("my_vkGetImageSubresourceLayout EXIT\n");
}

__attribute__((visibility("default")))
void vkGetImageSubresourceLayout(VkDevice device, void* image, const void* pSubresource, void* pLayout)
{
    my_vkGetImageSubresourceLayout(device, image, pSubresource, pLayout);
}

/*
 * Dynamic ARM64 device trampoline generator:
 * Unwraps x0 from struct WrappedDevice* (offset 8) to real_device and branches
 * directly to real_fn with all other arguments and stack intact.
 */
static char*  s_tramp_pool = NULL;
static size_t s_tramp_offset = 0;

#define CACHE_MAX 2048
#define TRAMP_POOL_SIZE (4 * 1024 * 1024)

struct TrampCacheEntry {
    char name[64];
    void* tramp;
};
static struct TrampCacheEntry s_cache[CACHE_MAX];
static int s_cache_count = 0;

/* Trampoline template:
 *   stp  x29, x30, [sp, #-16]!     ; save fp/lr (we'll bl, so x30 clobbers)
 *   mov  x29, sp
 *   mrs  x16, tpidr_el0            ; x16 = glibc TPIDR (caller's)
 *   str  x16, [sp, #-16]!          ; push caller TPIDR
 *   ldr  x16, <bionic_tpidr>       ; load saved bionic TPIDR
 *   msr  tpidr_el0, x16            ; switch to bionic TLS
 *   ldr  x0, [x0, #8]              ; unwrap WrappedDevice -> real_device (device-tramp only)
 *   ldr  x16, <real_fn>
 *   blr  x16                        ; call vendor driver (bionic context)
 *   ldr  x16, [sp], #16            ; pop caller TPIDR into x16
 *   msr  tpidr_el0, x16            ; restore glibc TLS
 *   ldp  x29, x30, [sp], #16       ; restore fp/lr
 *   ret
 *   .quad bionic_tpidr_ptr          ; literal pool
 *   .quad real_fn                   ;
 *
 * instance/physdev trampolines are the same minus the "ldr x0, [x0, #8]"
 * device unwrap.
 */

static uint64_t s_bionic_tpidr_cached = 0;

static void init_tpidr_cache(void)
{
    if (!s_bionic_tpidr_cached) {
        s_bionic_tpidr_cached = mini_linker_get_bionic_tpidr();
    }
}

static void* ensure_tramp_pool(void)
{
    if (!s_tramp_pool) {
        s_tramp_pool = (char*)mmap(NULL, TRAMP_POOL_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC,
                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (s_tramp_pool == MAP_FAILED) {
            perror("mmap tramp pool");
            return NULL;
        }
    }
    return s_tramp_pool;
}

/* Build a trampoline that swaps TPIDR_EL0 (host<->bionic) around a call
 * into the vendor driver, with an optional VkDevice-unwrap prefix. */
static void* make_tpidr_trampoline(void* real_fn, int do_device_unwrap)
{
    if (!real_fn) return NULL;
    init_tpidr_cache();
    if (!ensure_tramp_pool()) return real_fn;

    const size_t slot = 128;  /* 56 bytes code + 16 bytes literals, aligned to 16, within 128-byte slot */
    if (s_tramp_offset + slot > TRAMP_POOL_SIZE) {
        fprintf(stderr, "[shim] Trampoline pool exhausted!\n");
        return real_fn;
    }
    uint32_t* code = (uint32_t*)(s_tramp_pool + s_tramp_offset);
    s_tramp_offset += slot;

    int i = 0;
    /* Opcodes verified against the GNU assembler. */
    /* stp x29, x30, [sp, #-16]! */        code[i++] = 0xa9bf7bfd;
    /* mov x29, sp */                      code[i++] = 0x910003fd;
    /* mrs x16, TPIDR_EL0 */               code[i++] = 0xd53bd050;
    /* str x16, [sp, #-16]! */             code[i++] = 0xf81f0ff0;
    /* ldr x16, bionic_tpidr_literal */    code[i++] = 0x58000010;  /* imm19 patched below */
    /* msr TPIDR_EL0, x16 */               code[i++] = 0xd51bd050;
    if (do_device_unwrap) {
    /* cbz x0, skip_unwrap (+28) */            code[i++] = 0xb40000e0;
    /* ldr w16, [x0, #16] */                   code[i++] = 0xb9401010;
    /* mov w17, #0x4150 */                     code[i++] = 0x52882a11;
    /* movk w17, #0x5752, lsl #16 */           code[i++] = 0x72aaea51;
    /* cmp w16, w17 */                         code[i++] = 0x6b11021f;
    /* b.ne skip_unwrap (+8) */                code[i++] = 0x54000041;
    /* ldr x0, [x0, #8] */                     code[i++] = 0xf9400400;
    /* skip_unwrap: */
    }
    /* ldr x16, real_fn_literal */         code[i++] = 0x58000010;  /* imm19 patched below */
    /* blr x16 */                          code[i++] = 0xd63f0200;
    /* ldr x16, [sp], #16 */               code[i++] = 0xf84107f0;
    /* msr TPIDR_EL0, x16 */               code[i++] = 0xd51bd050;
    /* ldp x29, x30, [sp], #16 */          code[i++] = 0xa8c17bfd;
    /* ret */                              code[i++] = 0xd65f03c0;

    /* ARM64 LDR (literal) immediate: imm19 << 2 is byte offset from PC
     * of the ldr instruction. Patch imm19 field (bits 23..5). */
    int code_bytes = i * 4;
    int literal_off = (code_bytes + 15) & ~15;  /* align to 16 */
    uint64_t* literal = (uint64_t*)((char*)code + literal_off);
    literal[0] = s_bionic_tpidr_cached;
    literal[1] = (uint64_t)real_fn;

    /* patch the two "ldr x16, <literal>" instructions */
    /* first "ldr x16" is at code[4] (always). offset to literal[0] = literal_off - 16. */
    int ldr0_pc = 16;
    int ldr0_tgt = literal_off;
    int ldr0_imm19 = (ldr0_tgt - ldr0_pc) >> 2;
    code[4] = 0x58000010 | ((ldr0_imm19 & 0x7ffff) << 5);

    /* second "ldr x16" at code index: 6 w/o unwrap, 13 with unwrap (cbz + 5 insns + ldr) */
    int second_ldr_idx = do_device_unwrap ? 13 : 6;
    int ldr1_pc = second_ldr_idx * 4;
    int ldr1_tgt = literal_off + 8;
    int ldr1_imm19 = (ldr1_tgt - ldr1_pc) >> 2;
    code[second_ldr_idx] = 0x58000010 | ((ldr1_imm19 & 0x7ffff) << 5);

    __builtin___clear_cache((char*)code, (char*)code + slot);
    return (void*)code;
}

static void* make_device_trampoline(void* real_fn)
{
    return make_tpidr_trampoline(real_fn, 1);
}

static void* make_instance_trampoline(void* real_fn)
{
    /* Instance/PhysDev-taking functions need the SAME x0 unwrap as devices.
     * All dispatchable Vulkan handles have the same shape; our wrapper
     * structs all have `real` at offset 8. Use the unwrap variant. */
    return make_tpidr_trampoline(real_fn, 1);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!name) return (PFN_vkVoidFunction)0;
    SHIM_LOG("GDPA query '%s' (dev=%p)\n", name, device);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)my_vkGetDeviceQueue;
    if (strcmp(name, "vkGetDeviceQueue2") == 0 || strcmp(name, "vkGetDeviceQueue2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetDeviceQueue2;
    if (strcmp(name, "vkCreateSamplerYcbcrConversion") == 0 || strcmp(name, "vkCreateSamplerYcbcrConversionKHR") == 0)
        return (PFN_vkVoidFunction)my_vkCreateSamplerYcbcrConversion;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)my_vkCreateSampler;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)my_vkCreateDescriptorSetLayout;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)my_vkCreatePipelineLayout;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)my_vkCreateCommandPool;
    if (strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)my_vkCreateDescriptorPool;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)my_vkCreateShaderModule;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)my_vkAllocateDescriptorSets;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkCreateBuffer;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)my_vkGetBufferMemoryRequirements;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)my_vkAllocateMemory;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)my_vkBindBufferMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)my_vkMapMemory;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)my_vkCreateImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)my_vkGetImageMemoryRequirements;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)my_vkBindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)my_vkCreateImageView;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)my_vkAllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)my_vkFreeCommandBuffers;
    if (strcmp(name, "vkQueueSubmit") == 0)
        return (PFN_vkVoidFunction)my_vkQueueSubmit;
    if (strcmp(name, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)my_vkQueueWaitIdle;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)my_vkDeviceWaitIdle;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkResetCommandBuffer;
    if (strcmp(name, "vkCmdPipelineBarrier") == 0)
        return (PFN_vkVoidFunction)my_vkCmdPipelineBarrier;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
        return (PFN_vkVoidFunction)my_vkCmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyImage") == 0)
        return (PFN_vkVoidFunction)my_vkCmdCopyImage;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)my_vkCmdBindPipeline;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)my_vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCmdDispatch") == 0)
        return (PFN_vkVoidFunction)my_vkCmdDispatch;
    if (strcmp(name, "vkCmdResetQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkCmdResetQueryPool;
    if (strcmp(name, "vkCmdWriteTimestamp") == 0)
        return (PFN_vkVoidFunction)my_vkCmdWriteTimestamp;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)my_vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyPipeline;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyImage;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyImageView;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyBuffer;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)my_vkFreeMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)my_vkUnmapMemory;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyCommandPool;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)my_vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)my_vkDestroySemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)my_vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)my_vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)my_vkWaitForFences;
    if (strcmp(name, "vkCreateQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkCreateQueryPool;
    if (strcmp(name, "vkDestroyQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyQueryPool;
    if (strcmp(name, "vkGetQueryPoolResults") == 0)
        return (PFN_vkVoidFunction)my_vkGetQueryPoolResults;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)my_vkUpdateDescriptorSets;
    if (strcmp(name, "vkGetImageSubresourceLayout") == 0)
        return (PFN_vkVoidFunction)my_vkGetImageSubresourceLayout;

    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].name, name) == 0) {
            SHIM_LOG("GDPA CACHE HIT '%s' -> %p\n", name, s_cache[i].tramp);
            return (PFN_vkVoidFunction)s_cache[i].tramp;
        }
    }

    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) return (PFN_vkVoidFunction)0;

    void* real_fn = (void*)safe_gipa(s_last_instance, name);
    SHIM_LOG("GDPA real_fn for '%s' = %p\n", name, real_fn);
    if (!real_fn) return (PFN_vkVoidFunction)0;

    void* tramp = make_device_trampoline(real_fn);
    SHIM_LOG("GDPA tramp for '%s' = %p\n", name, tramp);
    if (s_cache_count < CACHE_MAX) {
        strncpy(s_cache[s_cache_count].name, name, sizeof(s_cache[s_cache_count].name) - 1);
        s_cache[s_cache_count].tramp = tramp;
        s_cache_count++;
    }
    return (PFN_vkVoidFunction)tramp;
}

/* Cache instance-level trampolines so repeated GIPA lookups return the same
 * trampoline for a given name (saves pool and makes behaviour deterministic). */
static struct TrampCacheEntry s_inst_cache[CACHE_MAX];
static int s_inst_cache_count = 0;

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
    if (!name) return (PFN_vkVoidFunction)0;
    SHIM_LOG("GPDPA query '%s' (inst=%p)\n", name, instance);

    VkInstance real_inst = unwrap_instance(instance);

    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)my_vkEnumerateDeviceExtensionProperties;
    if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)
        return (PFN_vkVoidFunction)my_vkEnumerateDeviceLayerProperties;
    if (strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0 ||
        strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
        return (PFN_vkVoidFunction)my_vkEnumeratePhysicalDeviceGroups;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceImageFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)my_vkCreateDevice;

    for (int i = 0; i < s_inst_cache_count; i++) {
        if (strcmp(s_inst_cache[i].name, name) == 0) {
            SHIM_LOG("GPDPA CACHE HIT '%s' -> %p\n", name, s_inst_cache[i].tramp);
            return (PFN_vkVoidFunction)s_inst_cache[i].tramp;
        }
    }

    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) return (PFN_vkVoidFunction)0;
    void* real = (void*)safe_gipa(real_inst, name);
    if (!real) return (PFN_vkVoidFunction)0;

    void* tramp = make_instance_trampoline(real);
    SHIM_LOG("GPDPA CACHE [%d]: %s real=%p tramp=%p\n", s_inst_cache_count, name, real, tramp);
    if (s_inst_cache_count < CACHE_MAX) {
        strncpy(s_inst_cache[s_inst_cache_count].name, name,
                sizeof(s_inst_cache[0].name) - 1);
        s_inst_cache[s_inst_cache_count].tramp = tramp;
        s_inst_cache_count++;
    }
    return (PFN_vkVoidFunction)tramp;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name) return (PFN_vkVoidFunction)0;
    SHIM_LOG("GIPA query '%s' (inst=%p)\n", name, instance);
    if (strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion") == 0)
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    if (strcmp(name, "vk_icdGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)my_vkCreateDevice;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)my_vkCreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)my_vkEnumeratePhysicalDevices;
    /* Non-handle first-arg entry points: route to our C wrappers directly
     * (they do their own TPIDR swap) so the trampoline doesn't try to
     * unwrap a non-handle pointer like uint32_t* / const char*. */
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)my_vkEnumerateDeviceExtensionProperties;
    if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)
        return (PFN_vkVoidFunction)my_vkEnumerateDeviceLayerProperties;
    if (strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0 ||
        strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
        return (PFN_vkVoidFunction)my_vkEnumeratePhysicalDeviceGroups;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceImageFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)my_vkGetDeviceQueue;
    if (strcmp(name, "vkGetDeviceQueue2") == 0 || strcmp(name, "vkGetDeviceQueue2KHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetDeviceQueue2;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkResetCommandBuffer;
    if (strcmp(name, "vkCmdPipelineBarrier") == 0)
        return (PFN_vkVoidFunction)my_vkCmdPipelineBarrier;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
        return (PFN_vkVoidFunction)my_vkCmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyImage") == 0)
        return (PFN_vkVoidFunction)my_vkCmdCopyImage;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)my_vkCmdBindPipeline;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)my_vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCmdDispatch") == 0)
        return (PFN_vkVoidFunction)my_vkCmdDispatch;
    if (strcmp(name, "vkCmdResetQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkCmdResetQueryPool;
    if (strcmp(name, "vkCmdWriteTimestamp") == 0)
        return (PFN_vkVoidFunction)my_vkCmdWriteTimestamp;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)my_vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyPipeline;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyImage;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyImageView;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyBuffer;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)my_vkFreeMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)my_vkUnmapMemory;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyCommandPool;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)my_vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)my_vkDestroySemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)my_vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)my_vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)my_vkWaitForFences;
    if (strcmp(name, "vkCreateQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkCreateQueryPool;
    if (strcmp(name, "vkDestroyQueryPool") == 0)
        return (PFN_vkVoidFunction)my_vkDestroyQueryPool;
    if (strcmp(name, "vkGetQueryPoolResults") == 0)
        return (PFN_vkVoidFunction)my_vkGetQueryPoolResults;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)my_vkUpdateDescriptorSets;
    if (strcmp(name, "vkGetImageSubresourceLayout") == 0)
        return (PFN_vkVoidFunction)my_vkGetImageSubresourceLayout;

    VkInstance real_inst = unwrap_instance(instance);

    for (int i = 0; i < s_inst_cache_count; i++) {
        if (strcmp(s_inst_cache[i].name, name) == 0) {
            SHIM_LOG("GIPA CACHE HIT '%s' -> %p\n", name, s_inst_cache[i].tramp);
            return (PFN_vkVoidFunction)s_inst_cache[i].tramp;
        }
    }

    if (!android_gipa) load_android_vulkan();
    if (!android_gipa) return (PFN_vkVoidFunction)0;
    void* real = (void*)safe_gipa(real_inst, name);
    if (!real) return (PFN_vkVoidFunction)0;

    void* tramp = make_instance_trampoline(real);
    SHIM_LOG("GIPA CACHE [%d]: %s real=%p tramp=%p\n", s_inst_cache_count, name, real, tramp);
    if (s_inst_cache_count < CACHE_MAX) {
        strncpy(s_inst_cache[s_inst_cache_count].name, name,
                sizeof(s_inst_cache[0].name) - 1);
        s_inst_cache[s_inst_cache_count].tramp = tramp;
        s_inst_cache_count++;
    }
    return (PFN_vkVoidFunction)tramp;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}
