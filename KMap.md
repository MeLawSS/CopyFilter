# KMap — Kernel-Mode Generic Hash Table

`KMap.h` / `KMap.c` implements a chaining hash table for Windows kernel drivers.
Key and value types and sizes are specified by the caller at runtime — no compile-time templates required.

---

## Features

| Feature | Description |
|---------|-------------|
| **Memory type** | Non-paged (`NonPagedPool`) or paged (`PagedPool`), selected at creation |
| **Synchronization** | Non-paged: `KSPIN_LOCK` (≤ DISPATCH_LEVEL); Paged: `FAST_MUTEX` (PASSIVE_LEVEL only) |
| **Dual API sets** | Locking (`KMapXxx`) and non-locking (`KMapXxxUnsafe`) variants for every operation |
| **Manual lock** | `KMapLock` / `KMapUnlock` for holding the lock across multiple operations atomically |
| **Node allocation** | `NPAGED_LOOKASIDE_LIST` / `PAGED_LOOKASIDE_LIST` to reduce overhead from frequent insert/delete |

`<ntddk.h>` or `<fltKernel.h>` must be included before `#include "KMap.h"`.

---

## Quick Start

```c
/* 1. Provide a hash function and an equality function */
static ULONG HashPid(const VOID *Key, ULONG TableSize)
{
    return (*(const ULONG *)Key) % TableSize;
}

static BOOLEAN EqualPid(const VOID *StoredKey, const VOID *LookupKey)
{
    return *(const ULONG *)StoredKey == *(const ULONG *)LookupKey;
}

/* 2. Create (non-paged, 64 buckets, key=ULONG, value=ULONG64) */
PKMAP g_Map = KMapCreate(64, sizeof(ULONG), sizeof(ULONG64),
                          'pMkK', TRUE, HashPid, EqualPid);

/* 3. Insert / find / remove */
ULONG pid = 1234; ULONG64 val = 0xdeadbeef;
KMapInsert(g_Map, &pid, &val);

ULONG64 out = 0;
if (KMapFind(g_Map, &pid, &out)) { /* out == 0xdeadbeef */ }

KMapRemove(g_Map, &pid, NULL);

/* 4. Destroy when the driver unloads */
KMapDestroy(g_Map);
g_Map = NULL;
```

---

## API Reference

### Lifecycle

```c
// Create. Must be called at PASSIVE_LEVEL. Returns NULL on allocation failure.
// Recommended prime BucketCount values: 17 / 53 / 97 / 257 / 521
PKMAP KMapCreate(ULONG BucketCount, ULONG KeySize, ULONG ValueSize,
                 ULONG PoolTag, BOOLEAN IsNonPaged,
                 KMAP_HASH_FN Hash, KMAP_EQUAL_FN Equal);

// Destroy. Must be called at PASSIVE_LEVEL. The pointer must not be used after this call.
VOID  KMapDestroy(PKMAP Map);
```

### Manual Lock

For operations that must execute atomically (e.g. "insert if not present"). Only `Unsafe` variants may be called while the lock is held.

```c
VOID KMapLock  (PKMAP Map, PKIRQL OldIrql);
VOID KMapUnlock(PKMAP Map, KIRQL  OldIrql);

// Example: atomic "insert if absent"
KIRQL irql;
KMapLock(g_Map, &irql);
if (!KMapContainsUnsafe(g_Map, &key)) {
    KMapInsertUnsafe(g_Map, &key, &value);
}
KMapUnlock(g_Map, irql);
```

### Thread-Safe Operations (auto-locking)

| Function | Description |
|----------|-------------|
| `KMapInsert(Map, Key, Value)` | Insert; returns `FALSE` if key already exists |
| `KMapInsertOrUpdate(Map, Key, Value, OldValue)` | Insert or overwrite; `OldValue` may be `NULL` |
| `KMapUpdate(Map, Key, Value, OldValue)` | Update existing node only; returns `FALSE` if key not found |
| `KMapRemove(Map, Key, OutValue)` | Remove; `OutValue` may be `NULL` |
| `KMapFind(Map, Key, OutValue)` | Find and copy value; `OutValue` may be `NULL` |
| `KMapContains(Map, Key)` | Check whether key exists |
| `KMapEnum(Map, Fn, Context)` | Enumerate all nodes (see below) |
| `KMapClear(Map)` | Remove all nodes (map structure is retained) |
| `KMapCount(Map)` | Return current node count (lock-free read) |

### Non-Thread-Safe Operations (Unsafe)

Same signatures as the locking variants, with the `Unsafe` suffix:

```
KMapInsertUnsafe  KMapInsertOrUpdateUnsafe  KMapUpdateUnsafe
KMapRemoveUnsafe  KMapFindUnsafe            KMapContainsUnsafe
KMapEnumUnsafe    KMapClearUnsafe
```

### Enumeration

```c
// Callback prototype: return FALSE to continue, TRUE to stop early
typedef BOOLEAN (*KMAP_ENUM_FN)(const VOID *Key, VOID *Value, VOID *Context);
```

- The callback may **modify `Value` in-place**, but **must not insert or remove nodes**
- `KMapEnum` (locking variant) executes the callback while holding the lock — do not call any locking API from within the callback

```c
// Example: enumerate and print all pids
static BOOLEAN PrintPid(const VOID *Key, VOID *Value, VOID *Context)
{
    UNREFERENCED_PARAMETER(Context);
    KernOutputInfo("pid=%lu val=%llu", *(ULONG *)Key, *(ULONG64 *)Value);
    return FALSE;
}
KMapEnum(g_Map, PrintPid, NULL);
```

---

## Callback Contracts

### KMAP_HASH_FN

```c
typedef ULONG (*KMAP_HASH_FN)(const VOID *Key, ULONG TableSize);
// Return value must be in [0, TableSize). The same Key must always return the same value.
```

Common implementations:

```c
// ULONG key (integer hash)
static ULONG HashUlong(const VOID *Key, ULONG TableSize) {
    ULONG k = *(const ULONG *)Key;
    k ^= k >> 16; k *= 0x45d9f3b; k ^= k >> 16;
    return k % TableSize;
}

// Case-insensitive char* key (djb2 variant)
static ULONG HashStringA(const VOID *Key, ULONG TableSize) {
    const CHAR *s = (const CHAR *)Key;
    ULONG hash = 5381; CHAR c;
    while ((c = *s++) != '\0') {
        if (c >= 'A' && c <= 'Z') c += 32;
        hash = ((hash << 5) + hash) ^ (ULONG)c;
    }
    return hash % TableSize;
}
```

### KMAP_EQUAL_FN

```c
typedef BOOLEAN (*KMAP_EQUAL_FN)(const VOID *StoredKey, const VOID *LookupKey);
// StoredKey: the key copy stored inside the node
// LookupKey: the key passed in for this lookup
```

---

## IRQL Constraints

| Operation | Non-paged Map | Paged Map |
|-----------|--------------|-----------|
| `KMapCreate` / `KMapDestroy` | PASSIVE_LEVEL | PASSIVE_LEVEL |
| Locking insert/remove/find/enum | ≤ DISPATCH_LEVEL | PASSIVE_LEVEL |
| `KMapLock` / `KMapUnlock` | ≤ DISPATCH_LEVEL | PASSIVE_LEVEL |
| `KMapCount` | any | any |
| `KMapXxxUnsafe` | ≤ DISPATCH_LEVEL | PASSIVE_LEVEL |

> Enumeration callbacks for a non-paged Map execute at DISPATCH_LEVEL — do not access paged memory inside the callback.

---

## Memory Layout

```
KMapCreate single allocation:
┌─────────────────────────────────────┐
│ KMAP (struct)                        │
│   Buckets ──────────────────────┐  │
├─────────────────────────────────│──┤
│ LIST_ENTRY[0]  ◄────────────────┘  │
│ LIST_ENTRY[1]                       │
│ ...                                 │
│ LIST_ENTRY[BucketCount-1]           │
└─────────────────────────────────────┘

Each node (single Lookaside allocation):
┌─────────────┬──────────────────────┬───────────────┐
│  LIST_ENTRY │  Key (ptr-aligned)   │  Value        │
└─────────────┴──────────────────────┴───────────────┘
```

---

## Notes

- `KMapDestroy` must be called at PASSIVE_LEVEL (requirement of `ExDeleteNPagedLookasideList`)
- Do not insert or remove nodes inside an enumeration callback — list iteration behaviour is undefined if the list is modified
- `KMapCount` is a lock-free read; it may be briefly inconsistent under high concurrency and should be treated as advisory
- Bucket count guideline: when expecting N nodes, choose a prime near N/4 as `BucketCount`
