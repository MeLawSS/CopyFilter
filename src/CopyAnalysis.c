
#include "CopyAnalysis.h"
#include "VolumeInfo.h"
#include "KernDebugLog.h"
#include "KMap.h"
#include "Macros.h"

#define ONE_MICROSECOND   (10)
#define ONE_MILLISECOND   (ONE_MICROSECOND*1000)
#define ONE_SECOND        (ONE_MILLISECOND*1000)

#define Wait_Interval_Second                2       // wait interval in seconds

typedef struct _File_Props {
    char filePath[MAX_PATH_K];

    STORAGE_BUS_TYPE phyDevBusType;
    DEVICE_TYPE deviceType;
    FLT_FILESYSTEM_TYPE fileSystemType;
} File_Props, * PFile_Props;

//copy behavior analysis data
typedef struct _Copy_Analysis_Data {

    File_Props srcProps;
    File_Props dstProps;

    ULONG pid;
    ULONG tid;

    PVOID bufferAddr;                   //shared buffer address

    LONGLONG timeStamp;                 //timestamp when this entry was created

} Copy_Analysis_Data, * PCopy_Analysis_Data;

#pragma data_seg("NONPAGE")
PKMAP					g_mapCopyAnalysis = NULL;			// stores Copy_Analysis_Data nodes, keyed by buffer address
#pragma data_seg()

static ULONG HashBufferAddr(const VOID* Key, ULONG TableSize)
{
    return (*(const ULONGLONG*)Key) % TableSize;
}

static BOOLEAN EqualBufferAddr(const VOID* StoredKey, const VOID* LookupKey)
{
    return *(const ULONGLONG*)StoredKey == *(const ULONGLONG*)LookupKey;
}

static BOOLEAN GetDeviceType(IN PDEVICE_OBJECT pDeviceObject, OUT DEVICE_TYPE* pDeviceType)
{
    BOOLEAN bRet = FALSE;
    PDEVICE_OBJECT pVolumeDeviceObject = NULL;

    do {
        if (pDeviceObject == NULL) {
            KernOutputError("pDeviceObject is NULL!");
            break;
        }

        pVolumeDeviceObject = IoGetDeviceAttachmentBaseRef(pDeviceObject);
        if (pVolumeDeviceObject == NULL) {
            KernOutputError("IoGetDeviceAttachmentBaseRef() failed, pVolumeDeviceObject is NULL!");
            break;
        }

        *pDeviceType = pVolumeDeviceObject->DeviceType;

        bRet = TRUE;

    } while (0);

    if (pVolumeDeviceObject != NULL) {
        ObDereferenceObject(pVolumeDeviceObject);
    }

    return bRet;
}

// Enumeration callback context: collect keys (buffer addresses) of expired nodes
typedef struct _EXPIRE_COLLECT_CTX {
    LONGLONG  curTimeStamp;
    PVOID    *pKeys;        // caller-allocated array; capacity holds up to capacity PVOIDs
    ULONG     count;        // number actually collected
    ULONG     capacity;     // maximum capacity of the pKeys array
} EXPIRE_COLLECT_CTX;

// KMapEnum callback: append expired node keys to ctx->pKeys without modifying the Map
static BOOLEAN CollectExpiredNode(
    _In_    const VOID *Key,
    _Inout_ VOID       *Value,
    _In_opt_ VOID      *Context)
{
    EXPIRE_COLLECT_CTX  *ctx  = (EXPIRE_COLLECT_CTX *)Context;
    PCopy_Analysis_Data  node = (PCopy_Analysis_Data)Value;

    if (ctx != NULL) {
        if (ctx->count < ctx->capacity && ((LONGLONG)(ONE_SECOND * Wait_Interval_Second) < ctx->curTimeStamp - node->timeStamp)) {
            ctx->pKeys[ctx->count++] = *(PVOID*)Key;
        }
    }


    return FALSE; // continue enumeration
}

static void ClearExpiredNode(LONGLONG nCurTimeStamp)
{
    static LONGLONG     nTimeStamp = 0;
    ULONG               nCount;
    PVOID*              pKeys;
    EXPIRE_COLLECT_CTX  ctx = { 0 };
    ULONG               i;

    if ((LONGLONG)(ONE_SECOND * Wait_Interval_Second) >= nCurTimeStamp - nTimeStamp) {
        return;
    }

    nTimeStamp = nCurTimeStamp;

    nCount = KMapCount(g_mapCopyAnalysis);
    if (nCount == 0)
        return;

    // allocate temporary key array (paged memory; this function is called at PASSIVE_LEVEL)
    pKeys = (PVOID *)ExAllocatePool2(POOL_FLAG_PAGED,
                                     (SIZE_T)nCount * sizeof(PVOID),
                                     PoolTag_CopyAnalysis);
    if (pKeys == NULL)
    {
        KernOutputError("ExAllocatePool2 failed, count=%lu", nCount);
        return;
    }

    // step 1: enumerate and collect expired node keys (lock held during enumeration, no insert/remove allowed)
    ctx.curTimeStamp = nCurTimeStamp;
    ctx.pKeys        = pKeys;
    ctx.count        = 0;
    ctx.capacity     = nCount;
    KMapEnum(g_mapCopyAnalysis, CollectExpiredNode, &ctx);

    // step 2: delete each expired node after enumeration (KMapRemove acquires the lock internally)
    for (i = 0; i < ctx.count; i++)
    {
        KMapRemove(g_mapCopyAnalysis, &pKeys[i], NULL);
    }

    KernOutputInfo("removed %lu expired node(s), remaining=%lu",
                   ctx.count, KMapCount(g_mapCopyAnalysis));

    ExFreePoolWithTag(pKeys, PoolTag_CopyAnalysis);
}

static STORAGE_BUS_TYPE GetBusType(IN PFLT_VOLUME pVolume, IN PDEVICE_OBJECT pDevObj) {
    Volume_Info volInfo = { 0 };
    
    if (!FindVolumeInfo(pVolume, &volInfo)) {
        AddVolumeInfo(pVolume, pDevObj);

        if (!FindVolumeInfo(pVolume, &volInfo)) {
            KernOutputError("FindVolumeInfo() failed!");
            return BusTypeUnknown;
        }
    }

    return volInfo.BusType;
}

// Fill path and filesystem-type fields in File_Props.
// Must be called at PASSIVE_LEVEL.
static VOID FillFileProps(
    _In_    PFLT_CALLBACK_DATA       pData,
    _In_    PCFLT_RELATED_OBJECTS    pFltObjects,
    _Out_   PFile_Props              pProps)
{
    POBJECT_NAME_INFORMATION    pDosName = NULL;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(pData);

    // filePath: IoQueryFileDosDeviceName returns "\??\C:\path\to\file" format,
    // which includes the full drive-letter path; no separate volume name lookup needed.
    status = IoQueryFileDosDeviceName(pFltObjects->FileObject, &pDosName);
    if (NT_SUCCESS(status))
    {
        UNICODE_STRING  nameStr  = pDosName->Name;
        ANSI_STRING     ansiPath = { 0 };
        ULONG           copyLen;

        // strip "\??\" prefix (4 wide chars = 8 bytes)
        if (nameStr.Length >= 4 * sizeof(WCHAR) &&
            nameStr.Buffer[0] == L'\\' &&
            nameStr.Buffer[1] == L'?'  &&
            nameStr.Buffer[2] == L'?'  &&
            nameStr.Buffer[3] == L'\\')
        {
            nameStr.Buffer        += 4;
            nameStr.Length        -= (USHORT)(4 * sizeof(WCHAR));
            nameStr.MaximumLength -= (USHORT)(4 * sizeof(WCHAR));
        }

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&ansiPath, &nameStr, TRUE)))
        {
            copyLen = min((ULONG)ansiPath.Length, MAX_PATH_K - 1);
            RtlCopyMemory(pProps->filePath, ansiPath.Buffer, copyLen);
            pProps->filePath[copyLen] = '\0';
            RtlFreeAnsiString(&ansiPath);
        }

        ExFreePool(pDosName);
    }
    else
    {
        KernOutputError("IoQueryFileDosDeviceName() failed, status=0x%08X", status);
    }

    // fileSystemType
    status = FltGetFileSystemType(pFltObjects->Instance, &pProps->fileSystemType);
    if (!NT_SUCCESS(status))
        KernOutputError("FltGetFileSystemType() failed, status=0x%08X", status);
}

static BOOLEAN CacheMatch(PVOID pBuffer, PCopy_Analysis_Data pRet)
{
    return KMapFind(g_mapCopyAnalysis, &pBuffer, pRet);
}


BOOLEAN InitCopyAnalysis()
{
    g_mapCopyAnalysis = KMapCreate(53, sizeof(PVOID), sizeof(Copy_Analysis_Data),
        PoolTag_CopyAnalysis, FALSE, HashBufferAddr, EqualBufferAddr);

    return g_mapCopyAnalysis == NULL ? FALSE : TRUE;
}

VOID UninitCopyAnalysis()
{
    KernOutputInfo("uninit.");
    KMapDestroy(g_mapCopyAnalysis);
}

BOOLEAN PreReadCache(_In_ PFLT_CALLBACK_DATA pData, _In_ PCFLT_RELATED_OBJECTS pFltObjects)
{
	BOOLEAN bRet = FALSE;
    LARGE_INTEGER curTime = { 0 };
    Copy_Analysis_Data struData = { 0 };

	do
	{
        // this is a public-facing API; validate all input parameters
        if (NULL == pData || NULL == pFltObjects)
            break;

        // not Buffered I/O
        // (may be Direct I/O; skip for now — handle if copy operations using Direct I/O are missed in later testing)
        if (pData->Iopb->Parameters.Read.ReadBuffer == NULL)
            break;

        // sequential I/O only
        if (FO_SEQUENTIAL_ONLY != (pFltObjects->FileObject->Flags & FO_SEQUENTIAL_ONLY))
            break;

        if ((&pData->Iopb->Parameters)->Read.ByteOffset.QuadPart != 0) {
            break;
        }

        if (!GetDeviceType(pFltObjects->FileObject->DeviceObject, &(struData.srcProps.deviceType)))
            break;

        
        struData.srcProps.phyDevBusType = GetBusType(pFltObjects->Volume, pFltObjects->FileObject->DeviceObject);

        FillFileProps(pData, pFltObjects, &struData.srcProps);
        struData.pid = FltGetRequestorProcessId(pData);
        struData.tid = HandleToUlong(PsGetCurrentThreadId());

        struData.bufferAddr = pData->Iopb->Parameters.Read.ReadBuffer;

        KeQuerySystemTime(&curTime);
        struData.timeStamp = curTime.QuadPart;
        ClearExpiredNode(struData.timeStamp); // periodic cache cleanup; reading files is inherently slow so this has negligible performance impact


        if (!KMapInsertOrUpdate(g_mapCopyAnalysis, &(struData.bufferAddr), &struData, NULL)) {
            KernOutputError("KMapInsertOrUpdate() failed!");
            break;
        }

        KernOutputInfo("Cache info. srcFilePath: %s srcBusType: %d BufferAddr: 0x%p TimeStamp: %llu",
            struData.srcProps.filePath, struData.srcProps.phyDevBusType, struData.bufferAddr, struData.timeStamp);

        bRet = TRUE;
	} while (0);

	return bRet;
}

BOOLEAN PreWriteMatch(_In_ PFLT_CALLBACK_DATA pData, _In_ PCFLT_RELATED_OBJECTS pFltObjects)
{
    BOOLEAN bRet = FALSE;
    Copy_Analysis_Data struData = { 0 };

    do
    {
        // this is a public-facing API; validate all input parameters
        if (NULL == pData || NULL == pFltObjects)
            break;

        // not Buffered I/O
        if (pData->Iopb->Parameters.Write.WriteBuffer == NULL)
            break;

        // sequential I/O only
        if (FO_SEQUENTIAL_ONLY != (pFltObjects->FileObject->Flags & FO_SEQUENTIAL_ONLY))
            break;

        if (pData->Iopb->Parameters.Read.ByteOffset.QuadPart != 0) {
            break;
        }

        // match write buffer against cached read
        if (!CacheMatch(pData->Iopb->Parameters.Write.WriteBuffer, &struData)) {
            break;
        }

        if (!GetDeviceType(pFltObjects->FileObject->DeviceObject, &(struData.dstProps.deviceType)))
            break;


        struData.dstProps.phyDevBusType = GetBusType(pFltObjects->Volume, pFltObjects->FileObject->DeviceObject);

        FillFileProps(pData, pFltObjects, &struData.dstProps);
        
        KernOutputInfo("file-copy recognized. srcFilePath: %s dstFilePath: %s srcFileSystem: %d srcDeviceType: %d srcBusType: %d dstFileSystem: %d dstDeviceType: %d dstBusType: %d MatchedBufferAddr: 0x%p TimeStamp: %llu",
            struData.srcProps.filePath, struData.dstProps.filePath, struData.srcProps.fileSystemType, struData.srcProps.deviceType, struData.srcProps.phyDevBusType, struData.dstProps.fileSystemType, struData.dstProps.deviceType, struData.dstProps.phyDevBusType, struData.bufferAddr, struData.timeStamp);

        if (!KMapRemove(g_mapCopyAnalysis, &struData.bufferAddr, NULL)) {
            KernOutputError("fail to remove node with key %p", struData.bufferAddr);
        }

        bRet = TRUE;
    } while (0);

    return bRet;
}