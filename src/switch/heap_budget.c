/* Switch: keep newlib's heap apart from the guest's shared-memory backing.
 *
 * hbmenu's loader (nx-hbloader) hands an NRO almost the whole process heap,
 * but the guest arena (toolkit win32_compat.c, Switch memory) is made of
 * kernel shared-memory objects, which come out of the same process quota.
 * So the heap is shrunk to a budget at start-up -- never below the loader's
 * prefix, which holds the NRO itself -- and the loader's size is restored at
 * exit, before it loads the next NRO.
 *
 * From the SSX Tricky Switch port (src/switch/heap_budget.c), checked on
 * hardware there in full-memory (title) mode.
 */
#ifdef __SWITCH__
#include <switch.h>
#include <stdint.h>

/* Host allocations: the software rasteriser's targets and texture cache, the
 * APU, file buffers, the log pipe. The guest's RAM, mirrors and apertures are
 * shared memory and not part of it. */
size_t __nx_heap_size = 512u * 1024u * 1024u;

Result ssx3NxHeapBudgetResult;
size_t ssx3NxHeapOriginalSize, ssx3NxHeapActualSize;
static int resized;
extern char *fake_heap_start, *fake_heap_end;

void __libnx_initheap(void)
{
    void *base = NULL;

    if (!envHasHeapOverride()) {
        ssx3NxHeapBudgetResult = svcSetHeapSize(&base, __nx_heap_size);
        if (R_FAILED(ssx3NxHeapBudgetResult))
            diagAbortWithResult(ssx3NxHeapBudgetResult);
        fake_heap_start = base;
        fake_heap_end = fake_heap_start + __nx_heap_size;
        ssx3NxHeapActualSize = __nx_heap_size;
        return;
    }

    uintptr_t start = (uintptr_t)envGetHeapOverrideAddr();
    size_t available = envGetHeapOverrideSize();
    fake_heap_start = (char *)start;
    fake_heap_end = fake_heap_start + available;
    ssx3NxHeapActualSize = available;

    u64 heap_base = 0;
    ssx3NxHeapBudgetResult = svcGetInfo(&heap_base, InfoType_HeapRegionAddress,
                                        CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(ssx3NxHeapBudgetResult))
        return;
    /* Only the documented nx-hbloader layout is resized: the override is
     * the free tail of one heap mapping that starts at the heap region. */
    MemoryInfo info;
    u32 page;
    ssx3NxHeapBudgetResult = svcQueryMemory(&info, &page, start);
    if (R_FAILED(ssx3NxHeapBudgetResult))
        return;
    if (start < heap_base || available > UINTPTR_MAX - start ||
        info.type != MemType_Heap || info.addr > start ||
        info.size > UINT64_MAX - info.addr || info.addr + info.size < start + available) {
        ssx3NxHeapBudgetResult = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return;
    }
    size_t prefix = start - heap_base;
    ssx3NxHeapOriginalSize = prefix + available;
    if (available <= __nx_heap_size)
        return;
    size_t requested = (prefix + __nx_heap_size + 0x1fffff) & ~(size_t)0x1fffff;
    if (requested >= ssx3NxHeapOriginalSize)
        return;
    ssx3NxHeapBudgetResult = svcSetHeapSize(&base, requested);
    if (R_FAILED(ssx3NxHeapBudgetResult))
        return;
    /* The heap base is fixed; everything the loader put below start stays. */
    if ((uintptr_t)base != heap_base)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
    resized = 1;
    fake_heap_end = (char *)base + requested;
    ssx3NxHeapActualSize = (uintptr_t)fake_heap_end - start;
}

Result ssx3NxHeapRestore(void)
{
    if (!resized)
        return 0;
    void *base = NULL;
    Result rc = svcSetHeapSize(&base, ssx3NxHeapOriginalSize);
    if (R_SUCCEEDED(rc))
        resized = 0;
    return rc;
}

void userAppExit(void)
{
    /* The guest's shared memory must be gone first (ssx3NxMemoryShutdown,
     * toolkit), or the quota it holds makes this fail. Never return to a
     * loader whose heap was not given back. */
    Result rc = ssx3NxHeapRestore();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);
}
#endif
