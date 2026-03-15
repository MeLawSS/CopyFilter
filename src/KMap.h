/*
 * KMap.h — Kernel-Mode Generic Hash Table
 *
 * Features:
 *   - Chaining hash table; key/value sizes are specified by the caller at runtime (not compile-time)
 *   - Separate code paths for paged and non-paged memory; non-paged uses KSPIN_LOCK
 *     (safe at DISPATCH_LEVEL), paged uses FAST_MUTEX (PASSIVE_LEVEL only)
 *   - Every API has a locking variant (KMapXxx) and a non-locking variant (KMapXxxUnsafe)
 *   - Manual Lock/Unlock support for performing multiple operations atomically while holding the lock
 *   - Nodes are managed by a Lookaside List to reduce allocation overhead on frequent insert/delete
 *
 * Prerequisite:
 *   The caller must #include <ntddk.h> or <fltKernel.h> before #include "KMap.h".
 *
 * Usage example:
 *
 *   // ULONG pid -> ULONG64 data (non-paged, auto-locking)
 *   static ULONG HashPid(const VOID *Key, ULONG Size) {
 *       return (*(const ULONG *)Key) % Size;
 *   }
 *   static BOOLEAN EqualPid(const VOID *A, const VOID *B) {
 *       return *(const ULONG *)A == *(const ULONG *)B;
 *   }
 *
 *   PKMAP g_Map = KMapCreate(64, sizeof(ULONG), sizeof(ULONG64),
 *                             'pMkK', TRUE, HashPid, EqualPid);
 *   ULONG pid = 1234; ULONG64 val = 0xdeadbeef;
 *   KMapInsert(g_Map, &pid, &val);
 *   KMapFind(g_Map, &pid, &val);
 *   KMapDestroy(g_Map);
 */

#pragma once
#include <ntddk.h>

/* --------------------------------------------------------------------------
 * Callback types
 * -------------------------------------------------------------------------- */

/*
 * Hash function: maps Key to a bucket index in [0, TableSize).
 * Must be deterministic; the same Key must always return the same value.
 */
typedef ULONG (*KMAP_HASH_FN)(
    _In_ const VOID *Key,
    _In_ ULONG       TableSize
    );

/*
 * Equality function: returns TRUE if the key stored in the node (StoredKey)
 * equals the lookup key (LookupKey).
 */
typedef BOOLEAN (*KMAP_EQUAL_FN)(
    _In_ const VOID *StoredKey,
    _In_ const VOID *LookupKey
    );

/*
 * Enumeration callback: called once per node in KMapEnum / KMapEnumUnsafe.
 * Return TRUE to stop enumeration early; return FALSE to continue.
 * Note: the callback executes while the lock is held — do not call any locking
 *       API variant on the same Map from within the callback.
 *       The callback may modify Value in-place, but must not insert or remove nodes.
 */
typedef BOOLEAN (*KMAP_ENUM_FN)(
    _In_    const VOID *Key,
    _Inout_ VOID       *Value,
    _In_opt_ VOID      *Context
    );

/* --------------------------------------------------------------------------
 * Map structure
 * -------------------------------------------------------------------------- */

typedef struct _KMAP {

    /* Configuration (read-only after initialization) */
    ULONG           BucketCount;
    ULONG           KeySize;            /* raw key size as specified by the caller */
    ULONG           KeySizeAligned;     /* key size rounded up to pointer alignment */
    ULONG           ValueSize;
    ULONG           PoolTag;
    BOOLEAN         IsNonPaged;
    KMAP_HASH_FN    Hash;
    KMAP_EQUAL_FN   Equal;

    /* Runtime state */
    ULONG           Count;

    /* Node allocator */
    union {
        NPAGED_LOOKASIDE_LIST   NP;
        PAGED_LOOKASIDE_LIST    P;
    } Lookaside;

    /* Synchronization primitive
     *   IsNonPaged = TRUE  → KSPIN_LOCK, usable at IRQL <= DISPATCH_LEVEL
     *   IsNonPaged = FALSE → FAST_MUTEX, usable at IRQL == PASSIVE_LEVEL only */
    union {
        KSPIN_LOCK  SpinLock;
        FAST_MUTEX  FastMutex;
    } Lock;

    /* Hash bucket array (allocated together with this struct, immediately following it) */
    PLIST_ENTRY     Buckets;

} KMAP, *PKMAP;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/*
 * Create and initialize a Map.
 *
 *   BucketCount  — number of hash buckets; a prime is recommended (e.g. 17/53/97/257/521).
 *   KeySize      — key size in bytes; the Map stores an internal copy of every key.
 *   ValueSize    — value size in bytes; the Map stores an internal copy of every value.
 *   PoolTag      — pool tag for kernel memory leak tracking.
 *   IsNonPaged   — TRUE: use non-paged memory (safe at DISPATCH_LEVEL);
 *                  FALSE: use paged memory (PASSIVE_LEVEL only).
 *   Hash         — hash function; must not be NULL.
 *   Equal        — equality function; must not be NULL.
 *
 * Returns NULL on allocation failure.
 * The returned PKMAP must eventually be freed with KMapDestroy.
 */
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
    );

/*
 * Remove all nodes and free the Map itself. The pointer must not be used after this call.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
KMapDestroy(
    _In_ _Post_ptr_invalid_ PKMAP Map
    );

/* --------------------------------------------------------------------------
 * Manual lock / unlock
 *
 * Use when multiple operations must execute atomically (e.g. "find, then conditionally insert").
 * Only Unsafe variants may be called while the lock is held; calling locking variants will deadlock.
 *
 *   OldIrql — for non-paged Maps, saves/restores the IRQL; pass NULL for paged Maps.
 * -------------------------------------------------------------------------- */

VOID
KMapLock(
    _In_    PKMAP  Map,
    _Out_   PKIRQL OldIrql
    );

VOID
KMapUnlock(
    _In_ PKMAP Map,
    _In_ KIRQL OldIrql
    );

/* --------------------------------------------------------------------------
 * Thread-safe operations (auto-locking)
 * -------------------------------------------------------------------------- */

/*
 * Insert a new node. Returns FALSE if the key already exists.
 */
BOOLEAN
KMapInsert(
    _In_ PKMAP      Map,
    _In_ const VOID *Key,
    _In_ const VOID *Value
    );

/*
 * Insert or update.
 *   Key absent  → insert; returns TRUE; OldValue (if non-NULL) is zeroed.
 *   Key present → overwrite value; returns TRUE; OldValue (if non-NULL) receives the old value.
 */
BOOLEAN
KMapInsertOrUpdate(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    );

/*
 * Update an existing node's value only. Returns FALSE if the key is not found.
 */
BOOLEAN
KMapUpdate(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    );

/*
 * Remove a node. OutValue (if non-NULL) receives the removed node's value.
 * Returns FALSE if the key is not found.
 */
BOOLEAN
KMapRemove(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    );

/*
 * Find a node and copy its value to OutValue (if non-NULL).
 * Returns FALSE if the key is not found.
 */
BOOLEAN
KMapFind(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    );

/*
 * Check whether Key exists.
 */
BOOLEAN
KMapContains(
    _In_ PKMAP      Map,
    _In_ const VOID *Key
    );

/*
 * Enumerate all nodes (in bucket order; no particular sort guarantee).
 * The callback executes while the lock is held; do not call locking API variants from within it.
 */
VOID
KMapEnum(
    _In_     PKMAP        Map,
    _In_     KMAP_ENUM_FN Fn,
    _In_opt_ VOID         *Context
    );

/*
 * Remove all nodes (retains the Map structure itself).
 */
VOID
KMapClear(
    _In_ PKMAP Map
    );

/*
 * Return the current node count.
 */
ULONG
KMapCount(
    _In_ PKMAP Map
    );

/* --------------------------------------------------------------------------
 * Non-thread-safe operations (Unsafe)
 *
 * The caller is responsible for concurrency safety, or must use these only in
 * single-threaded contexts or while holding the lock manually.
 * -------------------------------------------------------------------------- */

BOOLEAN
KMapInsertUnsafe(
    _In_ PKMAP      Map,
    _In_ const VOID *Key,
    _In_ const VOID *Value
    );

BOOLEAN
KMapInsertOrUpdateUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    );

BOOLEAN
KMapUpdateUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _In_      const VOID *Value,
    _Out_opt_ VOID       *OldValue
    );

BOOLEAN
KMapRemoveUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    );

BOOLEAN
KMapFindUnsafe(
    _In_      PKMAP      Map,
    _In_      const VOID *Key,
    _Out_opt_ VOID       *OutValue
    );

BOOLEAN
KMapContainsUnsafe(
    _In_ PKMAP      Map,
    _In_ const VOID *Key
    );

VOID
KMapEnumUnsafe(
    _In_     PKMAP        Map,
    _In_     KMAP_ENUM_FN Fn,
    _In_opt_ VOID         *Context
    );

VOID
KMapClearUnsafe(
    _In_ PKMAP Map
    );
