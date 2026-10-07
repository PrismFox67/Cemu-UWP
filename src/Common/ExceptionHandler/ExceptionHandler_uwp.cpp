// Minimal crash logging for UWP builds. dbghelp (minidumps, symbolized stack traces) isn't available to UWP apps,
// so this only writes the exception, registers and the general info into log.txt
#include "Common/precompiled.h"
#include "ExceptionHandler.h"
#include "Common/version.h"

#include <Windows.h>

static LONG WINAPI _CemuUnhandledExceptionFilter(EXCEPTION_POINTERS* e)
{
	if (!CrashLog_Create())
		return EXCEPTION_CONTINUE_SEARCH;
	cemuLog_writePlainToLog(fmt::format("\nCrashlog for {}\n", BUILD_VERSION_WITH_NAME_STRING));
	if (e && e->ExceptionRecord)
		cemuLog_writePlainToLog(fmt::format("Exception 0x{:08x} at 0x{:x}\n", (uint32)e->ExceptionRecord->ExceptionCode, (uint64)e->ExceptionRecord->ExceptionAddress));
	if (e && e->ContextRecord)
	{
		const CONTEXT* c = e->ContextRecord;
		cemuLog_writePlainToLog(fmt::format("RAX={:016x} RBX={:016x} RCX={:016x} RDX={:016x}\n", c->Rax, c->Rbx, c->Rcx, c->Rdx));
		cemuLog_writePlainToLog(fmt::format("RSP={:016x} RBP={:016x} RDI={:016x} RSI={:016x}\n", c->Rsp, c->Rbp, c->Rdi, c->Rsi));
		cemuLog_writePlainToLog(fmt::format("R8 ={:016x} R9 ={:016x} R10={:016x} R11={:016x}\n", c->R8, c->R9, c->R10, c->R11));
		cemuLog_writePlainToLog(fmt::format("R12={:016x} R13={:016x} R14={:016x} R15={:016x}\n", c->R12, c->R13, c->R14, c->R15));
	}
	CrashLog_SetOutputChannels(false, true);
	ExceptionHandler_LogGeneralInfo();
	CrashLog_SetOutputChannels(true, true);
	cemuLog_waitForFlush();
	return EXCEPTION_CONTINUE_SEARCH;
}

void ExceptionHandler_Init()
{
	SetUnhandledExceptionFilter(_CemuUnhandledExceptionFilter);
}
