#include "KMap.h"

/* --------------------------------------------------------------------------
 * Internal node structure
 *
 * Memory layout (single contiguous allocation):
 *   [KMAP_NODE][Key data (KeySizeAligned bytes)][Value data (ValueSize bytes)]
 *
 * Allocated from a Lookaside List; each allocation has a fixed size of KMAP_NODE_ALLOC_SIZE(map).
 * -------------------------------------------------------------------------- */

typedef struct _KMAP_NODE {
    LIST_ENTRY Link;
} KMAP_NODE, *PKMAP_NODE;

/* round n up to pointer-width alignment */
#define KMAP_PTR_SIZE               sizeof(VOID *)
#define KMAP_ALIGN_UP(n)            (((n) + KMAP_PTR_SIZE - 1) & ~(KMAP_PTR_SIZE - 1))

#define KMAP_NODE_KEY(node) \
    ((VOID *)((UCHAR *)(node) + sizeof(KMAP_NODE)))

#define KMAP_NODE_VALUE(map, node) \
    ((VOID *)((UCHAR *)(node) + sizeof(KMAP_NODE) + (map)->KeySizeAligned))

#define KMAP_NODE_ALLOC_SIZE(map) \
    (sizeof(KMAP_NODE) + (map)->KeySizeAligned + (map)->ValueSize)

/* ============================================================
 * Internal utility functions
 * ============================================================ */

/* Allocate a node and zero-initialize it */
static PKMAP_NODE
KMapiAllocNode(
    _In_ PKMAP Map
    )
{
    PKMAP_NODE node;

    if (Map->IsNonPaged) {
        node = (PKMAP_NODE)ExAllocateFromNPagedLookasideList(&Map->Lookaside.NP);
    } else {
        node = (PKMAP_NODE)ExAllocateFromPagedLookasideList(&Map->Lookaside.P);
    }

    if (node != NULL) {
        RtlZeroMemory(node, KMAP_NODE_ALLOC_SIZE(Map));
    }

    return node;
}

/* Free a node */
static VOID
KMapiFreeNode(
    _In_ PKMAP      Map,
    _In_ PKMAP_NODE Node
    )
{
    if (Map->IsNonPaged) {
        ExFreeToNPagedLookasideList(&Map->Lookaside.NP, Node);
    } else {
        ExFreeToPagedLookasideList(&Map->Lookaside.P, Node);
    }
}

/*
 * Find the LIST_ENTRY in the bucket chain that matches Key.
 * Returns NULL if not found. Does not acquire the lock; caller is responsible for synchronization.
 */
static PLIST_ENTRY
KMapiFindEntry(
    _In_ PKMAP      Map,
    _In_ const VOID *Key
    )
{
    ULONG       bucket = Map->Hash(Key, Map->BucketCount);
    PLIST_ENTRY head   = &Map->Buckets[bucket];
    PLIST_ENTRY entry;

    for (entry = head->Flink; entry != head; entry = entry->Flink) {
        PKMAP_NODE node = CONTAINING_RECORD(entry, KMAP_NODE, Link);
        if (Map->Equal(KMAP_NODE_KEY(node), Key)) {
            return entry;
        }
    }
    return NULL;
}

/*
 * Core insert logic (no lock held).
 *   Update = FALSE → return FALSE if the key already exists (insert-only)
 *   Update = TRUE  → overwrite the value if the key already exists (insert-or-update)
 *   UpdateOnly     → return FALSE if the key does not exist (update-only)
 */
static BOOLEAN
KMapiInsertCore(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _In_      BOOLEAN    Update,
    _In_      BOOLEAN    UpdateOnly,
    _Out_opt_ VOID       *OldValue
    )
{
    ULONG       bucket;
    PLIST_ENTRY existing;
    PKMAP_NODE  node;

    existing = KMapiFindEntry(Map, Key);

    if (existing != NULL) {
        /* Key already exists */
        if (!Update) {
            return FALSE;   /* insert-only mode: reject */
        }
        node = CONTAINING_RECORD(existing, KMAP_NODE, Link);
        if (OldValue != NULL) {
            RtlCopyMemory(OldValue, KMAP_NODE_VALUE(Map, node), Map->ValueSize);
        }
        RtlCopyMemory(KMAP_NODE_VALUE(Map, node), Value, Map->ValueSize);
        return TRUE;
    }

    /* Key does not exist */
    if (UpdateOnly) {
        return FALSE;       /* update-only mode: reject */
    }

    node = KMapiAllocNode(Map);
    if (node == NULL) {
        return FALSE;
    }

    RtlCopyMemory(KMAP_NODE_KEY(node),       Key,   Map->KeySize);
    RtlCopyMemory(KMAP_NODE_VALUE(Map, node), Value, Map->ValueSize);

    bucket = Map->Hash(Key, Map->BucketCount);
    InsertTailList(&Map->Buckets[bucket], &node->Link);
    Map->Count++;

    if (OldValue != NULL) {
        RtlZeroMemory(OldValue, Map->ValueSize);
    }
    return TRUE;
}

/* 核心删除逻辑（不持锁） */
static BOOLEAN
KMapiRemoveCore(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    PLIST_ENTRY existing = KMapiFindEntry(Map, Key);
    PKMAP_NODE  node;

    if (existing == NULL) {
        return FALSE;
    }

    node = CONTAINING_RECORD(existing, KMAP_NODE, Link);

    if (OutValue != NULL) {
        RtlCopyMemory(OutValue, KMAP_NODE_VALUE(Map, node), Map->ValueSize);
    }

    RemoveEntryList(existing);
    KMapiFreeNode(Map, node);
    Map->Count--;
    return TRUE;
}

/* 核心查找逻辑（不持锁） */
static BOOLEAN
KMapiFindCore(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    PLIST_ENTRY existing = KMapiFindEntry(Map, Key);
    PKMAP_NODE  node;

    if (existing == NULL) {
        return FALSE;
    }

    if (OutValue != NULL) {
        node = CONTAINING_RECORD(existing, KMAP_NODE, Link);
        RtlCopyMemory(OutValue, KMAP_NODE_VALUE(Map, node), Map->ValueSize);
    }
    return TRUE;
}

/* 核心清空逻辑（不持锁） */
static VOID
KMapiClearCore(
    _In_ PKMAP Map
    )
{
    ULONG i;

    for (i = 0; i < Map->BucketCount; i++) {
        PLIST_ENTRY head = &Map->Buckets[i];
        while (!IsListEmpty(head)) {
            PKMAP_NODE node = CONTAINING_RECORD(RemoveHeadList(head), KMAP_NODE, Link);
            KMapiFreeNode(Map, node);
        }
    }
    Map->Count = 0;
}

/* 核心枚举逻辑（不持锁） */
static VOID
KMapiEnumCore(
    _In_     PKMAP        Map,
    _In_     KMAP_ENUM_FN Fn,
    _In_opt_ VOID         *Context
    )
{
    ULONG i;

    for (i = 0; i < Map->BucketCount; i++) {
        PLIST_ENTRY head  = &Map->Buckets[i];
        PLIST_ENTRY entry = head->Flink;

        while (entry != head) {
            PKMAP_NODE  node = CONTAINING_RECORD(entry, KMAP_NODE, Link);
            PLIST_ENTRY next = entry->Flink;    /* save next before the callback in case of unexpected node access */

            if (Fn(KMAP_NODE_KEY(node), KMAP_NODE_VALUE(Map, node), Context)) {
                return;     /* callback returned TRUE: stop early */
            }
            entry = next;
        }
    }
}

/* --------------------------------------------------------------------------
 * 加锁 / 解锁辅助
 * -------------------------------------------------------------------------- */

static VOID
KMapiAcquire(
    _In_  PKMAP  Map,
    _Out_ PKIRQL OldIrql
    )
{
    if (Map->IsNonPaged) {
        KeAcquireSpinLock(&Map->Lock.SpinLock, OldIrql);
    } else {
        ExAcquireFastMutex(&Map->Lock.FastMutex);
        *OldIrql = PASSIVE_LEVEL;
    }
}

static VOID
KMapiRelease(
    _In_ PKMAP Map,
    _In_ KIRQL OldIrql
    )
{
    if (Map->IsNonPaged) {
        KeReleaseSpinLock(&Map->Lock.SpinLock, OldIrql);
    } else {
        ExReleaseFastMutex(&Map->Lock.FastMutex);
    }
}

/* ============================================================
 * 公开 API 实现
 * ============================================================ */

/* ---------- 生命周期 ---------- */

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
PKMAP
KMapCreate(
    _In_ ULONG          BucketCount,
    _In_ ULONG          KeySize,
    _In_ ULONG          ValueSize,
    _In_ ULONG          PoolTag,
    _In_ BOOLEAN        IsNonPaged,
    _In_ KMAP_HASH_FN   Hash,
    _In_ KMAP_EQUAL_FN  Equal
    )
{
    PKMAP       map;
    ULONG       keySizeAligned;
    ULONG       nodeSize;
    ULONG       mapAllocSize;
    POOL_TYPE   poolType;
    ULONG       i;

    if (BucketCount == 0 || KeySize == 0 || ValueSize == 0 ||
        Hash == NULL || Equal == NULL) {
        return NULL;
    }

    keySizeAligned = KMAP_ALIGN_UP(KeySize);
    poolType       = IsNonPaged ? POOL_FLAG_NON_PAGED : POOL_FLAG_PAGED;

    /* Allocate KMAP struct and bucket array in a single allocation; buckets follow the struct */
    mapAllocSize = sizeof(KMAP) + BucketCount * sizeof(LIST_ENTRY);
    map = (PKMAP)ExAllocatePool2(POOL_FLAG_NON_PAGED, mapAllocSize, PoolTag);
    if (map == NULL) {
        return NULL;
    }

    RtlZeroMemory(map, mapAllocSize);

    map->BucketCount    = BucketCount;
    map->KeySize        = KeySize;
    map->KeySizeAligned = keySizeAligned;
    map->ValueSize      = ValueSize;
    map->PoolTag        = PoolTag;
    map->IsNonPaged     = IsNonPaged;
    map->Hash           = Hash;
    map->Equal          = Equal;
    map->Count          = 0;
    map->Buckets        = (PLIST_ENTRY)((UCHAR *)map + sizeof(KMAP));

    for (i = 0; i < BucketCount; i++) {
        InitializeListHead(&map->Buckets[i]);
    }

    /* node size = list entry + key (aligned) + value */
    nodeSize = (ULONG)KMAP_NODE_ALLOC_SIZE(map);

    if (IsNonPaged) {
        ExInitializeNPagedLookasideList(&map->Lookaside.NP,
                                        NULL, NULL, 0,
                                        nodeSize, PoolTag, 0);
        KeInitializeSpinLock(&map->Lock.SpinLock);
    } else {
        ExInitializePagedLookasideList(&map->Lookaside.P,
                                       NULL, NULL, 0,
                                       nodeSize, PoolTag, 0);
        ExInitializeFastMutex(&map->Lock.FastMutex);
    }

    return map;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
KMapDestroy(
    _In_ _Post_ptr_invalid_ PKMAP Map
    )
{
    if (Map == NULL) {
        return;
    }

    KMapClearUnsafe(Map);

    if (Map->IsNonPaged) {
        ExDeleteNPagedLookasideList(&Map->Lookaside.NP);
    } else {
        ExDeletePagedLookasideList(&Map->Lookaside.P);
    }

    ExFreePoolWithTag(Map, Map->PoolTag);
}

/* ---------- 手动锁 ---------- */

VOID
KMapLock(
    _In_  PKMAP  Map,
    _Out_ PKIRQL OldIrql
    )
{
    KMapiAcquire(Map, OldIrql);
}

VOID
KMapUnlock(
    _In_ PKMAP Map,
    _In_ KIRQL OldIrql
    )
{
    KMapiRelease(Map, OldIrql);
}

/* ---------- 线程安全操作 ---------- */

BOOLEAN
KMapInsert(
    _In_ PKMAP      Map,
    _In_ const VOID *Key,
    _In_ const VOID *Value
    )
{
    KIRQL   irql;
    BOOLEAN result;
    KMapiAcquire(Map, &irql);
    result = KMapiInsertCore(Map, Key, Value, FALSE, FALSE, NULL);
    KMapiRelease(Map, irql);
    return result;
}

BOOLEAN
KMapInsertOrUpdate(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    )
{
    KIRQL   irql;
    BOOLEAN result;
    KMapiAcquire(Map, &irql);
    result = KMapiInsertCore(Map, Key, Value, TRUE, FALSE, OldValue);
    KMapiRelease(Map, irql);
    return result;
}

BOOLEAN
KMapUpdate(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    )
{
    KIRQL   irql;
    BOOLEAN result;
    KMapiAcquire(Map, &irql);
    result = KMapiInsertCore(Map, Key, Value, TRUE, TRUE, OldValue);
    KMapiRelease(Map, irql);
    return result;
}

BOOLEAN
KMapRemove(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    KIRQL   irql;
    BOOLEAN result;
    KMapiAcquire(Map, &irql);
    result = KMapiRemoveCore(Map, Key, OutValue);
    KMapiRelease(Map, irql);
    return result;
}

BOOLEAN
KMapFind(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    KIRQL   irql;
    BOOLEAN result;
    KMapiAcquire(Map, &irql);
    result = KMapiFindCore(Map, Key, OutValue);
    KMapiRelease(Map, irql);
    return result;
}

BOOLEAN
KMapContains(
    _In_ PKMAP      Map,
    _In_ const VOID *Key
    )
{
    return KMapFind(Map, Key, NULL);
}

VOID
KMapEnum(
    _In_     PKMAP        Map,
    _In_     KMAP_ENUM_FN Fn,
    _In_opt_ VOID         *Context
    )
{
    KIRQL irql;
    KMapiAcquire(Map, &irql);
    KMapiEnumCore(Map, Fn, Context);
    KMapiRelease(Map, irql);
}

VOID
KMapClear(
    _In_ PKMAP Map
    )
{
    KIRQL irql;
    KMapiAcquire(Map, &irql);
    KMapiClearCore(Map);
    KMapiRelease(Map, irql);
}

ULONG
KMapCount(
    _In_ PKMAP Map
    )
{
    return Map->Count;
}

/* ---------- Unsafe 操作 ---------- */

BOOLEAN
KMapInsertUnsafe(
    _In_ PKMAP      Map,
    _In_ const VOID *Key,
    _In_ const VOID *Value
    )
{
    return KMapiInsertCore(Map, Key, Value, FALSE, FALSE, NULL);
}

BOOLEAN
KMapInsertOrUpdateUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    )
{
    return KMapiInsertCore(Map, Key, Value, TRUE, FALSE, OldValue);
}

BOOLEAN
KMapUpdateUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    )
{
    return KMapiInsertCore(Map, Key, Value, TRUE, TRUE, OldValue);
}

BOOLEAN
KMapRemoveUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    return KMapiRemoveCore(Map, Key, OutValue);
}

BOOLEAN
KMapFindUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    )
{
    return KMapiFindCore(Map, Key, OutValue);
}

BOOLEAN
KMapContainsUnsafe(
    _In_ PKMAP      Map,
    _In_ const VOID *Key
    )
{
    return KMapiFindCore(Map, Key, NULL);
}

VOID
KMapEnumUnsafe(
    _In_     PKMAP        Map,
    _In_     KMAP_ENUM_FN Fn,
    _In_opt_ VOID         *Context
    )
{
    KMapiEnumCore(Map, Fn, Context);
}

VOID
KMapClearUnsafe(
    _In_ PKMAP Map
    )
{
    KMapiClearCore(Map);
}
