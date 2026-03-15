#pragma once
#include <ntddstor.h>

typedef struct _Volume_Info {

    PFLT_VOLUME pVolume;         // logical volume object
    PDEVICE_OBJECT pDevObj;      // physical device object associated with the logical volume

    STORAGE_BUS_TYPE BusType;
}Volume_Info, * PVolume_Info;

/**************************************************************/
// Initialize the volume info table.
// Parameters: none
// Returns: TRUE on success, FALSE on failure.
/**************************************************************/
BOOLEAN InitVolumeInfo();

/**************************************************************/
// Uninitialize the volume info table.
// Parameters: none
// Returns: none.
/**************************************************************/
VOID UninitVolumeInfo();

/**************************************************************/
// Add volume info.
// Parameters:
//		pVolume[IN]  - logical volume object
//		pDevObj[IN]  - volume physical device object
// Returns: TRUE on success, FALSE on failure.
/**************************************************************/
BOOLEAN AddVolumeInfo(PFLT_VOLUME pVolume, PDEVICE_OBJECT pDevObj);

/**************************************************************/
// Remove volume info.
// Parameters:
//		pVolume[IN]  - logical volume object
// Returns: TRUE on success, FALSE on failure.
/**************************************************************/
BOOLEAN RemoveVolumeInfo(PFLT_VOLUME pVolume);


/**************************************************************/
// params:
//		pVolume[IN]-logical volume object
//		pVolInfo[OUT]-buffer for receiving volume info.
// return: TRUE - found，FALSE - not found。
/**************************************************************/
BOOLEAN FindVolumeInfo(PFLT_VOLUME pVolume, __out PVolume_Info pVolInfo);

/**************************************************************/
// Check whether the physical storage device underlying a logical volume is portable.
// Parameters:
//		pVolume[IN]  - logical volume object
//		pDevObj[IN]  - volume physical device object
// Returns: TRUE if the device is portable, FALSE otherwise.
/**************************************************************/
BOOLEAN IsVolumeUnderlyingDevicePortable(PFLT_VOLUME pVolume, PDEVICE_OBJECT pDevObj);