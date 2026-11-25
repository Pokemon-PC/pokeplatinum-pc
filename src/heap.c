#include "heap.h"

#include <nitro.h>
#include <string.h>

#include "comm_manager.h"
#include "error_message_reset.h"

typedef struct HeapInfo {
    NNSFndHeapHandle *heapHandles;
    NNSFndHeapHandle *parentHeapHandles;
    void **subHeapRawPtrs;
    u16 *numMemBlocks;
    u8 *heapIdxs;
    u16 totalNumHeaps;
    u16 nTemplates;
    u16 maxHeaps;
    u16 unallocatedHeapID;
} HeapInfo;

typedef struct MemoryBlock {
    u8 filler_00[12];
    u32 heapID : 8;
    u32 filler_0D : 24;
} MemoryBlock;

static HeapInfo sHeapInfo;

static BOOL CreateHeapInternal(enum HeapID parent, enum HeapID child, u32 size, s32 alignment);

#ifdef PLATFORM_PC
// PC port: heap sizes were tuned for the DS, where pointers are 4 bytes. On
// the 64-bit PC build every structure holding pointers is larger, so all
// heaps get four times the room (the PC main arena is larger, hw_pc.h).
#define PC_HEAP_SIZE(sz) ((sz) * 4)
#else
#define PC_HEAP_SIZE(sz) (sz)
#endif

void Heap_InitSystem(const HeapParam *templates, u32 nTemplates, u32 totalNumHeaps, u32 preSize)
{
    void *ptr;
    u32 i;
    u32 usableHeaps = nTemplates + 24;

    if (totalNumHeaps < usableHeaps) {
        totalNumHeaps = usableHeaps;
    }

    if (preSize != 0) {
        // force align
        while (preSize % 4 != 0) {
            preSize++;
        }
        OS_AllocFromMainArenaLo(preSize, 4);
    }

    sHeapInfo.heapHandles = OS_AllocFromMainArenaLo(
        (usableHeaps + 1) * sizeof(NNSFndHeapHandle)
            + usableHeaps * sizeof(NNSFndHeapHandle)
            + usableHeaps * sizeof(void *)
            + totalNumHeaps * sizeof(u16)
            + totalNumHeaps,
        4);
    sHeapInfo.parentHeapHandles = sHeapInfo.heapHandles + (usableHeaps + 1);
    sHeapInfo.subHeapRawPtrs = (void **)(sHeapInfo.parentHeapHandles + usableHeaps);
    sHeapInfo.numMemBlocks = (u16 *)(sHeapInfo.subHeapRawPtrs + usableHeaps);
    sHeapInfo.heapIdxs = (u8 *)(sHeapInfo.numMemBlocks + totalNumHeaps);
    sHeapInfo.totalNumHeaps = (u16)totalNumHeaps;
    sHeapInfo.nTemplates = (u16)nTemplates;
    sHeapInfo.unallocatedHeapID = (u16)usableHeaps;
    sHeapInfo.maxHeaps = (u16)usableHeaps;

    for (i = 0; i < nTemplates; i++) {
        switch (templates[i].arena) {
        case OS_ARENA_MAIN:
        default:
#ifdef PLATFORM_PC
            // PC port: larger heaps.
            ptr = OS_AllocFromMainArenaLo(PC_HEAP_SIZE(templates[i].size), 4);
#else
            ptr = OS_AllocFromMainArenaLo(templates[i].size, 4);
#endif
            break;
        case OS_ARENA_MAINEX:
#ifdef PLATFORM_PC
            // PC port: larger heaps.
            ptr = OS_AllocFromMainExArenaHi(PC_HEAP_SIZE(templates[i].size), 4);
#else
            ptr = OS_AllocFromMainExArenaHi(templates[i].size, 4);
#endif
            break;
        }

        if (ptr != NULL) {
#ifdef PLATFORM_PC
            // PC port: larger heaps.
            sHeapInfo.heapHandles[i] = NNS_FndCreateExpHeap(ptr, PC_HEAP_SIZE(templates[i].size));
#else
            sHeapInfo.heapHandles[i] = NNS_FndCreateExpHeap(ptr, templates[i].size);
#endif
            sHeapInfo.heapIdxs[i] = (u8)i;
        } else {
            GF_ASSERT(FALSE);
        }
    }

    for (i = nTemplates; i < usableHeaps + 1; i++) {
        sHeapInfo.heapHandles[i] = NNS_FND_HEAP_INVALID_HANDLE;
        sHeapInfo.heapIdxs[i] = (u8)sHeapInfo.unallocatedHeapID;
    }

    while (i < totalNumHeaps) {
        sHeapInfo.heapIdxs[i++] = (u8)sHeapInfo.unallocatedHeapID;
    }

    for (i = 0; i < totalNumHeaps; i++) {
        sHeapInfo.numMemBlocks[i] = 0;
    }
}

static s32 FindFirstAvailableHeapHandle(void)
{
    s32 i;

    for (i = sHeapInfo.nTemplates; i < sHeapInfo.maxHeaps; i++) {
        if (sHeapInfo.heapHandles[i] == NNS_FND_HEAP_INVALID_HANDLE) {
            return i;
        }
    }

    return -1;
}

BOOL Heap_Create(enum HeapID parent, enum HeapID child, u32 size)
{
    return CreateHeapInternal(parent, child, size, 4);
}

BOOL Heap_CreateAtEnd(enum HeapID parent, enum HeapID child, u32 size)
{
    return CreateHeapInternal(parent, child, size, -4);
}

static BOOL CreateHeapInternal(enum HeapID parent, enum HeapID child, u32 size, s32 alignment)
{
#ifdef PLATFORM_PC
    // PC port: larger heaps (child heaps too).
    GF_ASSERT(OS_GetProcMode() != OS_PROCMODE_IRQ);
    size = PC_HEAP_SIZE(size);

    u8 *ptr = sHeapInfo.heapIdxs;
#else
    GF_ASSERT(OS_GetProcMode() != OS_PROCMODE_IRQ);

    u8 *ptr = sHeapInfo.heapIdxs;
#endif
    if (sHeapInfo.unallocatedHeapID == ptr[child]) {
        NNSFndHeapHandle parentHeap = sHeapInfo.heapHandles[ptr[parent]];
        if (parentHeap != NNS_FND_HEAP_INVALID_HANDLE) {
            void *newHeapAddr = NNS_FndAllocFromExpHeapEx(parentHeap, size, alignment);
            if (newHeapAddr != NULL) {
                s32 i = FindFirstAvailableHeapHandle();
                if (i >= 0) {
                    sHeapInfo.heapHandles[i] = NNS_FndCreateExpHeap(newHeapAddr, size);

                    if (sHeapInfo.heapHandles[i] != NNS_FND_HEAP_INVALID_HANDLE) {
                        sHeapInfo.parentHeapHandles[i] = parentHeap;
                        sHeapInfo.subHeapRawPtrs[i] = newHeapAddr;
                        sHeapInfo.heapIdxs[child] = (u8)i;

                        return TRUE;
                    } else {
                        GF_ASSERT(FALSE);
                    }
                } else {
                    GF_ASSERT(FALSE);
                }
            } else {
                GF_ASSERT(FALSE);
            }
        } else {
            GF_ASSERT(FALSE);
        }
    } else {
        GF_ASSERT(FALSE);
    }
    return FALSE;
}

void Heap_Destroy(enum HeapID heapID)
{
    GF_ASSERT(OS_GetProcMode() != OS_PROCMODE_IRQ);

    NNSFndHeapHandle handle = sHeapInfo.heapHandles[sHeapInfo.heapIdxs[heapID]];

    if (handle != NNS_FND_HEAP_INVALID_HANDLE) {
        NNS_FndDestroyExpHeap(handle);

        u8 index = sHeapInfo.heapIdxs[heapID];
        NNSFndHeapHandle parentHeap = sHeapInfo.parentHeapHandles[index];
        void *childRaw = sHeapInfo.subHeapRawPtrs[index];
        if (parentHeap != NNS_FND_HEAP_INVALID_HANDLE && childRaw != NULL) {
            NNS_FndFreeToExpHeap(parentHeap, childRaw);
        } else {
            GF_ASSERT(FALSE);
        }

        sHeapInfo.heapHandles[sHeapInfo.heapIdxs[heapID]] = NULL;
        sHeapInfo.parentHeapHandles[sHeapInfo.heapIdxs[heapID]] = NULL;
        sHeapInfo.subHeapRawPtrs[sHeapInfo.heapIdxs[heapID]] = NULL;

        sHeapInfo.heapIdxs[heapID] = (u8)sHeapInfo.unallocatedHeapID;
    }
}

static void *AllocFromHeapInternal(NNSFndHeapHandle heap, u32 size, s32 alignment, enum HeapID heapID)
{
    GF_ASSERT(heap != NNS_FND_HEAP_INVALID_HANDLE);

    OSIntrMode intrMode = OS_DisableInterrupts();
    size += sizeof(MemoryBlock);
    void *ptr = NNS_FndAllocFromExpHeapEx(heap, size, alignment);

    OS_RestoreInterrupts(intrMode);
    if (ptr != NULL) {
        ((MemoryBlock *)ptr)->heapID = heapID;

        ptr += sizeof(MemoryBlock);
    }

    return ptr;
}

#ifdef PLATFORM_PC
// PC port: debug checker (PPL_HEAPCHECK=1, src/platform_pc/nitro/nitro_pc.c):
// walks every heap's block lists; on damage, names the used block that ends
// where the damaged header starts (the likely overflow) and the code that
// allocated it. Returns FALSE when a heap is damaged.
#include <stdio.h>
#include <stdlib.h>
void pc_log(const char *fmt, ...);
void pc_log_backtrace(const char *why);
extern int gPcHeapCheck;
#define PC_MBLOCK_USED_SIG 0x5544
#define PC_MBLOCK_FREE_SIG 0x4652
typedef struct {
    NNSiFndHeapHead head;
    NNSiFndExpHeapHead exp;
} PcExpHeap;

static BOOL PcCheckBlockList(int idx, PcExpHeap *heap, NNSiFndExpMBlockList *list, u16 sig)
{
    u8 *lo = heap->head.heapStart, *hi = heap->head.heapEnd;
    NNSiFndExpHeapMBlockHead *prev = NULL;
    for (NNSiFndExpHeapMBlockHead *b = list->head; b != NULL; prev = b, b = b->pMBHeadNext) {
        BOOL ok = (u8 *)b >= lo && (u8 *)b < hi && b->signature == sig && b->pMBHeadPrev == prev
            && (u8 *)(b + 1) + b->blockSize <= hi;
        if (ok) {
            continue;
        }
        u8 *bad = prev == NULL ? (u8 *)list->head : (u8 *)prev->pMBHeadNext;
        pc_log("heapcheck: heap #%d (%p-%p) damaged %s block at %p (after %p)", idx, lo, hi,
            sig == PC_MBLOCK_USED_SIG ? "used" : "free", bad, prev);
        for (NNSiFndExpHeapMBlockHead *u = heap->exp.mbUsedList.head; u != NULL && (u8 *)u >= lo && (u8 *)u < hi; u = u->pMBHeadNext) {
            u8 *end = (u8 *)(u + 1) + u->blockSize;
            if (end <= bad && end + 64 > bad) {
                void *ra;
                memcpy(&ra, u + 1, sizeof ra);
                pc_log("heapcheck:   used block %p..%p (%u bytes) allocated from %p", (void *)(u + 1), end,
                    (unsigned)u->blockSize, ra);
            }
        }
        return FALSE;
    }
    return TRUE;
}

// On an allocation failure: the largest used blocks of the heap, with the
// allocation site recorded by Heap_Alloc (other allocators leave garbage).
static void PcDumpLargestBlocks(enum HeapID heapID)
{
    PcExpHeap *heap = (PcExpHeap *)sHeapInfo.heapHandles[sHeapInfo.heapIdxs[heapID]];
    if (heap == NULL) {
        return;
    }
    for (int pass = 0; pass < 5; pass++) {
        NNSiFndExpHeapMBlockHead *best = NULL;
        static NNSiFndExpHeapMBlockHead *shown[5];
        for (NNSiFndExpHeapMBlockHead *u = heap->exp.mbUsedList.head; u != NULL; u = u->pMBHeadNext) {
            BOOL already = FALSE;
            for (int k = 0; k < pass; k++) {
                already |= shown[k] == u;
            }
            if (!already && (best == NULL || u->blockSize > best->blockSize)) {
                best = u;
            }
        }
        if (best == NULL) {
            break;
        }
        shown[pass] = best;
        void *ra;
        memcpy(&ra, best + 1, sizeof ra);
        pc_log("heap %d: used block of %u bytes at %p (site %p)", (int)heapID, (unsigned)best->blockSize, (void *)(best + 1), ra);
    }
}

BOOL pc_heap_check(void)
{
    for (int i = 0; i < sHeapInfo.maxHeaps; i++) {
        PcExpHeap *heap = (PcExpHeap *)sHeapInfo.heapHandles[i];
        if (heap == NULL || heap->head.signature != 0x45585048) { // 'EXPH'
            continue;
        }
        if (!PcCheckBlockList(i, heap, &heap->exp.mbUsedList, PC_MBLOCK_USED_SIG)
            || !PcCheckBlockList(i, heap, &heap->exp.mbFreeList, PC_MBLOCK_FREE_SIG)) {
            return FALSE;
        }
    }
    return TRUE;
}

// Who allocated a block (pc_heap_check): kept in MemoryBlock's filler.
// PPL_HEAPLOG=N logs every allocation from heap N (size, caller, free space).
static void *PcNoteAllocSite(void *ptr, void *ra)
{
    static int logHeap = -2;
    if (logHeap == -2) {
        const char *e = getenv("PPL_HEAPLOG");
        logHeap = e != NULL ? atoi(e) : -1;
    }
    if (ptr != NULL && logHeap >= 0 && ((MemoryBlock *)((u8 *)ptr - sizeof(MemoryBlock)))->heapID == (u32)logHeap) {
        NNSiFndExpHeapMBlockHead *mb = (NNSiFndExpHeapMBlockHead *)((u8 *)ptr - sizeof(MemoryBlock)) - 1;
        pc_log("heaplog: heap %d +%u bytes at %p from %p, %u free", logHeap, (unsigned)mb->blockSize, ptr, ra,
            (unsigned)HeapExp_FndGetTotalFreeSize(logHeap));
    }
    if (ptr != NULL) {
        memcpy((u8 *)ptr - sizeof(MemoryBlock), &ra, sizeof ra);
        if (gPcHeapCheck == 1 && !pc_heap_check()) {
            pc_log_backtrace("heapcheck (alloc)");
            gPcHeapCheck = 2;
        }
    }
    return ptr;
}
#endif

#ifdef PLATFORM_PC
// PC port: say where an allocation failed.
static void AllocFail(void)
{
    pc_log_backtrace("alloc-fail");
    if (CommManager_IsInitialized()) {
#else
static void AllocFail(void)
{
    if (CommManager_IsInitialized()) {
#endif
        ErrorMessageReset_PrintErrorAndReset();
    }
}

void *Heap_Alloc(u32 heapID, u32 size)
{
    void *ptr = NULL;

    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
#ifdef PLATFORM_PC
        // PC port: note the allocation site (heap checker).
        ptr = PcNoteAllocSite(AllocFromHeapInternal(sHeapInfo.heapHandles[index], size, 4, heapID), __builtin_return_address(0));
        if (ptr == NULL) {
            pc_warn("Heap_Alloc(heap %d, %u bytes) failed: %u bytes free", (int)heapID, (unsigned)size, (unsigned)HeapExp_FndGetTotalFreeSize(heapID));
            PcDumpLargestBlocks(heapID);
        }
    }
#else
        ptr = AllocFromHeapInternal(sHeapInfo.heapHandles[index], size, 4, heapID);
    }
#endif

    if (ptr != NULL) {
        sHeapInfo.numMemBlocks[heapID]++;
    } else {
        AllocFail();
    }

    return ptr;
}

void *Heap_AllocAtEnd(u32 heapID, u32 size)
{
    void *ptr = NULL;

    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
#ifdef PLATFORM_PC
        // PC port: note the allocation site (heap checker).
        ptr = PcNoteAllocSite(AllocFromHeapInternal(sHeapInfo.heapHandles[index], size, -4, heapID), __builtin_return_address(0));
        if (ptr == NULL) {
            pc_warn("Heap_Alloc(heap %d, %u bytes) failed: %u bytes free", (int)heapID, (unsigned)size, (unsigned)HeapExp_FndGetTotalFreeSize(heapID));
            PcDumpLargestBlocks(heapID);
        }
    }
#else
        ptr = AllocFromHeapInternal(sHeapInfo.heapHandles[index], size, -4, heapID);
    }
#endif

    if (ptr != NULL) {
        sHeapInfo.numMemBlocks[heapID]++;
    } else {
        AllocFail();
    }

    return ptr;
}

void Heap_Free(void *ptr)
{
    ptr -= sizeof(MemoryBlock);
    enum HeapID heapID = (enum HeapID)((MemoryBlock *)ptr)->heapID;

    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
        NNSFndHeapHandle heap = sHeapInfo.heapHandles[index];

        GF_ASSERT(heap != NNS_FND_HEAP_INVALID_HANDLE);

        if (sHeapInfo.numMemBlocks[heapID] == 0) {
            GF_heap_c_dummy_return_true(heapID);
        }

        GF_ASSERT(sHeapInfo.numMemBlocks[heapID] != 0);

        sHeapInfo.numMemBlocks[heapID]--;

        OSIntrMode intrMode = OS_DisableInterrupts();
        NNS_FndFreeToExpHeap(heap, ptr);
        OS_RestoreInterrupts(intrMode);
        return;
    }

    GF_ASSERT(FALSE);
}

void Heap_FreeExplicit(u32 heapID, void *ptr)
{
    GF_ASSERT(OS_GetProcMode() != OS_PROCMODE_IRQ);

    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
        NNSFndHeapHandle heap = sHeapInfo.heapHandles[index];

        GF_ASSERT(heap != NNS_FND_HEAP_INVALID_HANDLE);

        ptr -= sizeof(MemoryBlock);
        GF_ASSERT(((MemoryBlock *)ptr)->heapID == heapID);

        NNS_FndFreeToExpHeap(heap, ptr);
        GF_ASSERT(sHeapInfo.numMemBlocks[heapID] != 0);
        sHeapInfo.numMemBlocks[heapID]--;
        return;
    }

    GF_ASSERT(FALSE);
}

u32 HeapExp_FndGetTotalFreeSize(u32 heapID)
{
    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
        return NNS_FndGetTotalFreeSizeForExpHeap(sHeapInfo.heapHandles[index]);
    }

    GF_ASSERT(FALSE);
    return 0;
}

void HeapExp_FndInitAllocator(NNSFndAllocator *pAllocator, u32 heapID, int alignment)
{
    if ((u32)heapID < sHeapInfo.totalNumHeaps) {
        u8 index = sHeapInfo.heapIdxs[heapID];
        NNS_FndInitAllocatorForExpHeap(pAllocator, sHeapInfo.heapHandles[index], alignment);
        return;
    }

    GF_ASSERT(FALSE);
}

void Heap_Realloc(void *ptr, u32 newSize)
{
    GF_ASSERT(OS_GetProcMode() != OS_PROCMODE_IRQ);

    newSize += sizeof(MemoryBlock);
    ptr -= sizeof(MemoryBlock);

    if (NNS_FndGetSizeForMBlockExpHeap(ptr) >= newSize) {
        u32 heapID = ((MemoryBlock *)ptr)->heapID;
        u8 index = sHeapInfo.heapIdxs[heapID];

        NNS_FndResizeForMBlockExpHeap(sHeapInfo.heapHandles[index], ptr, newSize);
        return;
    }
    GF_ASSERT(FALSE);
}

BOOL GF_heap_c_dummy_return_true(u32 heapID)
{
    return TRUE;
}
