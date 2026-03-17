
#include <fltkernel.h>
#include "KernDebugLog.h"
#include "KMap.h"
#include "PoolTags.h"
#include "VolumeInfo.h"


#pragma data_seg("NONPAGE")
PKMAP					g_mapVolumeInfo = NULL;			// store Volume_Info nodes，key is volume object address
#pragma data_seg()

static ULONG HashVolume(const VOID* Key, ULONG TableSize)
{
    return (*(const ULONGLONG*)Key) % TableSize;
}

static BOOLEAN EqualVolume(const VOID* StoredKey, const VOID* LookupKey)
{
    return *(const ULONGLONG*)StoredKey == *(const ULONGLONG*)LookupKey;
}

_Success_(return)
static BOOLEAN QueryVolumeStorageBusType(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_  STORAGE_BUS_TYPE* BusType
)
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK ioStatusBlock;
    PIRP irp;
    STORAGE_PROPERTY_QUERY query;
    PVOID buffer;
    ULONG bufferLength;

    do {
        // Initialize the query for the storage device property
        RtlZeroMemory(&query, sizeof(STORAGE_PROPERTY_QUERY));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        // Allocate memory for the query result
        bufferLength = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 256;
        buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, bufferLength, PoolTag_VolumeInfo);
        if (buffer == NULL)
        {
            KernOutputError("ExAllocatePool2() failed!");
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(buffer, bufferLength);

        // Initialize the event to wait for the completion of the request
        KeInitializeEvent(&event, NotificationEvent, FALSE);

        // Build the IRP for the property query
        irp = IoBuildDeviceIoControlRequest(
            IOCTL_STORAGE_QUERY_PROPERTY,
            DeviceObject,
            &query,
            sizeof(STORAGE_PROPERTY_QUERY),
            buffer,
            bufferLength,
            FALSE,
            &event,
            &ioStatusBlock
        );

        if (irp == NULL)
        {
            KernOutputError("IoBuildDeviceIoControlRequest() failed!");
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // Send the IRP and wait for the completion
        status = IoCallDriver(DeviceObject, irp);
        if (status == STATUS_PENDING)
        {
            if (NT_SUCCESS(KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL))) {
                status = ioStatusBlock.Status;
            } else {
                KernOutputError("KeWaitForSingleObject() failed!");
				status = STATUS_UNSUCCESSFUL;
            }
            
        }

        if (NT_SUCCESS(status))
        {
            // Extract the bus type from the storage device descriptor
            STORAGE_DEVICE_DESCRIPTOR* descriptor = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
            if (descriptor->BusType == BusTypeUnknown) // Check if bus type is valid
            {
                status = STATUS_UNSUCCESSFUL;
            }
            else
            {
                RtlCopyMemory(BusType, &descriptor->BusType, sizeof(STORAGE_BUS_TYPE));
            }
        }
    } while (0);

    // Clean up
    if(buffer != NULL)
        ExFreePool(buffer);

    return NT_SUCCESS(status);
}


BOOLEAN InitVolumeInfo() {
    g_mapVolumeInfo = KMapCreate(53, sizeof(ULONGLONG), sizeof(Volume_Info),
        PoolTag_VolumeInfo, FALSE, HashVolume, EqualVolume);
    return g_mapVolumeInfo == NULL ? FALSE : TRUE;
}

VOID UninitVolumeInfo()
{
    KernOutputInfo("enter.");
    KMapDestroy(g_mapVolumeInfo);
}

BOOLEAN AddVolumeInfo(PFLT_VOLUME pVolume, PDEVICE_OBJECT pDevObj) {
    BOOLEAN bRet = FALSE;
    Volume_Info volInfo = { 0 };

    do {
        if (pVolume == NULL)
        {
            KernOutputError("pVolume == NULL!");
            break;
        }
        volInfo.pVolume = pVolume;
        volInfo.pDevObj = pDevObj;

        if (!QueryVolumeStorageBusType(volInfo.pDevObj, &(volInfo.BusType)))
        {
            KernOutputError("QueryVolumeStorageBusType() failed!");
            break;
        }

        bRet = KMapInsert(g_mapVolumeInfo, &pVolume, &volInfo);

        if (!bRet)
        {
            KernOutputError("KMapInsert() failed!");
        }
    } while (0);

    return bRet;
}

BOOLEAN RemoveVolumeInfo(PFLT_VOLUME pVolume) {
    BOOLEAN bRet = FALSE;

    do {
        KMapRemove(g_mapVolumeInfo, &pVolume, NULL);
        if (!bRet)
        {
            KernOutputError("RemoveVolumeInfo() failed! non-existed key.");
        }
    } while (0);

    return bRet;
}

BOOLEAN FindVolumeInfo(PFLT_VOLUME pVolume, __out PVolume_Info pVolInfo) {
    if (KMapFind(g_mapVolumeInfo, &pVolume, pVolInfo))
    {
        return TRUE;
    } else {
        return FALSE;
    }
}

BOOLEAN IsVolumeUnderlyingDevicePortable(PFLT_VOLUME pVolume, PDEVICE_OBJECT pDevObj) {
    BOOLEAN bPortable = FALSE;
    Volume_Info volInfo = { 0 };
    KIRQL irql;

    do {
        KMapLock(g_mapVolumeInfo, &irql);
        if (!KMapFindUnsafe(g_mapVolumeInfo, &pVolume, &volInfo)) {
            AddVolumeInfo(pVolume, pDevObj);
            if (!KMapFindUnsafe(g_mapVolumeInfo, &pVolume, &volInfo)) {
                KernOutputError("failed to find volume info!");
                break;
            }
        }
        KMapUnlock(g_mapVolumeInfo, irql);

        KernOutputInfo("pVolume: 0x%p!", pVolume);

        bPortable = volInfo.BusType == BusTypeUsb || volInfo.BusType == BusTypeSd || volInfo.BusType == BusTypeMmc;
    } while (0);

    return bPortable;
}