
#pragma once
#include <ntifs.h>
#include <fltKernel.h>

/**************************************************************/
// Initialize the copy behavior analysis module
// Parameters:
//      None
// Return value:
//      TRUE - success, FALSE - failure
/**************************************************************/
BOOLEAN InitCopyAnalysis();

/**************************************************************/
// Uninitialize the copy behavior analysis module
// Parameters:
//      None
// Return value:
//      None
/**************************************************************/
VOID UninitCopyAnalysis();

/**************************************************************/
// Cache a read event
// Parameters:
//      pData[IN]       - PFLT_CALLBACK_DATA
//      pFltObjects[IN] - PCFLT_RELATED_OBJECTS
// Return value:
//      TRUE - success, FALSE - failure
/**************************************************************/
BOOLEAN PreReadCache(_In_ PFLT_CALLBACK_DATA pData, _In_ PCFLT_RELATED_OBJECTS pFltObjects);

/**************************************************************/
// Match a write event
// Parameters:
//      pData[IN]       - PFLT_CALLBACK_DATA
//      pFltObjects[IN] - PCFLT_RELATED_OBJECTS
// Return value:
//      TRUE - success, FALSE - failure
/**************************************************************/
BOOLEAN PreWriteMatch(_In_ PFLT_CALLBACK_DATA pData, _In_ PCFLT_RELATED_OBJECTS pFltObjects);