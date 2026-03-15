
// KernDebugLog.h : header file

#pragma once

// USAGE:
//#include "KernDebugLog.h"
//#pragma data_seg("NONPAGE")
//USING_Kern_DEBUG;
//#pragma data_seg()
//
// KernOutputInfo( "Name:%s Count:%d", loggerName, count );
// KernOutputError( "Name:%s Count:%d", loggerName, count );

#include "Macros.h"
#include <memory.h>

// global switch for kernel debug log
extern ULONG g_nDebug;
extern char g_strFileName[];

#define MAX_FILENAME_LEN 128
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#define USING_Kern_DEBUG	\
	ULONG g_nDebug = 1;

// DbgPrint and DbgPrintEx can be called at IRQL<=DIRQL. 
// However, Unicode format codes (%C, %S, %lc, %ls, %wc, %ws, and %wZ) can be used only at IRQL=PASSIVE_LEVEL.
// Also, because the debugger uses interprocess interrupts (IPIs) to communicate with other processors, 
// calling DbgPrint at IRQL>DIRQL can cause deadlocks.

#define KernOutput(_level_, _format_, ...)																\
    if (g_nDebug)																						\
    {																									\
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, 											\
                   _level_ ## DRIVER_NAME ## "!In %s(), " ## _format_ ## " {%s:%u}" ## "\n",			\
                   __FUNCTION__,																		\
                   ##__VA_ARGS__,																		\
                   __FILENAME__,																		\
                   __LINE__);																			\
    }

#define KernOutputError(_format_, ...)               KernOutput("[ERROR] ", _format_, __VA_ARGS__)
#define KernOutputInfo(_format_, ...)                KernOutput("[INFO] ", _format_, __VA_ARGS__)

//#define KernOutputError(_format_, ...)               (void)0
//#define KernOutputInfo(_format_, ...)                (void)0