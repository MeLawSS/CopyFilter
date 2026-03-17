#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>
#include "KernDebugLog.h"
#include "KMap.h"
#include "VolumeInfo.h"
#include "CopyAnalysis.h"

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

//---------------------------------------------------------------------------
//      Global variables
//---------------------------------------------------------------------------

#pragma data_seg("NONPAGE")
USING_Kern_DEBUG;
#pragma data_seg()

typedef struct _COPY_FILTER_DATA {

    //
    //  The filter handle that results from a call to
    //  FltRegisterFilter.
    //

    PFLT_FILTER FilterHandle;

} COPY_FILTER_DATA, *PCOPY_FILTER_DATA;


/*************************************************************************
    Prototypes for the startup and unload routines used for
    this Filter.

    Implementation in CopyFilter.c
*************************************************************************/

DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    );

NTSTATUS
CopyUnload (
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

FLT_PREOP_CALLBACK_STATUS
CopyPreRead (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS
CopyPreWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

VOID CopyInstanceTeardownCallback(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Reason
);

//
//  Structure that contains all the global data structures
//  used throughout NullFilter.
//

COPY_FILTER_DATA CopyFilterData;

//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, CopyUnload)
#pragma alloc_text(PAGE, CopyPreRead)
#pragma alloc_text(PAGE, CopyPreWrite)
#endif


//
//  This defines what we want to filter with FltMgr
//

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_READ,  0, CopyPreRead,  NULL },
    { IRP_MJ_WRITE, 0, CopyPreWrite, NULL },
    { IRP_MJ_OPERATION_END }
};

CONST FLT_REGISTRATION FilterRegistration = {

    sizeof( FLT_REGISTRATION ),         //  Size
    FLT_REGISTRATION_VERSION,           //  Version
    0,                                  //  Flags

    NULL,                               //  Context
    Callbacks,                          //  Operation callbacks

    CopyUnload,                         //  FilterUnload

    NULL,                               //  InstanceSetup
    NULL,                               //  InstanceQueryTeardown
    NULL,                               //  InstanceTeardownStart
    NULL,                               //  InstanceTeardownComplete

    NULL,                               //  GenerateFileName
    NULL,                               //  GenerateDestinationFileName
    NULL                                //  NormalizeNameComponent

};


/*************************************************************************
    Filter initialization and unload routines.
*************************************************************************/

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    This is the initialization routine for this miniFilter driver. This
    registers the miniFilter with FltMgr and initializes all
    its global data structures.

Arguments:

    DriverObject - Pointer to driver object created by the system to
        represent this driver.
    RegistryPath - Unicode string identifying where the parameters for this
        driver are located in the registry.

Return Value:

    Returns STATUS_SUCCESS.

--*/
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER( RegistryPath );

	KernOutputInfo("CopyFilter is being loaded!");

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    
    //
    //  Register with FltMgr
    //

    status = FltRegisterFilter( DriverObject,
                                &FilterRegistration,
                                &CopyFilterData.FilterHandle );

    FLT_ASSERT( NT_SUCCESS( status ) );

    if (NT_SUCCESS( status )) {

        //
        //  Start filtering i/o
        //

        status = FltStartFiltering( CopyFilterData.FilterHandle );

        if (!NT_SUCCESS(status)) {
            KernOutputInfo("FltStartFiltering() failed!");
            FltUnregisterFilter(CopyFilterData.FilterHandle);
        } else {
            KernOutputInfo("CopyFilter starts Filtering!");
        }
    }

    do
    {
        status = STATUS_UNSUCCESSFUL;

        if (!InitVolumeInfo()) {
            KernOutputInfo("InitVolumeInfo() failed!");
            break;
        }

        if (!InitCopyAnalysis()) {
            KernOutputInfo("InitCopyAnalysis() failed!");
            break;
        }
        
        status = STATUS_SUCCESS;
    } while (0);

    return status;
}

NTSTATUS
CopyUnload (
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
/*++

Routine Description:

    This is the unload routine for this miniFilter driver. This is called
    when the minifilter is about to be unloaded. We can fail this unload
    request if this is not a mandatory unloaded indicated by the Flags
    parameter.

Arguments:

    Flags - Indicating if this is a mandatory unload.

Return Value:

    Returns the final status of this operation.

--*/
{
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    UninitCopyAnalysis();
    UninitVolumeInfo();

    FltUnregisterFilter( CopyFilterData.FilterHandle );

    return STATUS_SUCCESS;
}

VOID CopyInstanceTeardownCallback(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Reason
)
{
    UNREFERENCED_PARAMETER(Reason);

    if (!RemoveVolumeInfo(FltObjects->Volume)) {
        KernOutputError("RemoveVolumeInfo() failed! pVolume: 0x%p", FltObjects->Volume);
    }
}


FLT_PREOP_CALLBACK_STATUS
CopyPreRead (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{

    PAGED_CODE();
    UNREFERENCED_PARAMETER( CompletionContext );

    do
    {
        if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
            break;
        }

        PreReadCache(Data, FltObjects);
    } while (0);


    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
CopyPreWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{

    PAGED_CODE();
    UNREFERENCED_PARAMETER(CompletionContext);

    do
    {
        if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
            break;
        }

        PreWriteMatch(Data, FltObjects);
    } while (0);

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

