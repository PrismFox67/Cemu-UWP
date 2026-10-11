// Minimal crash logging for UWP builds. dbghelp (minidumps, symbolized stack traces) isn't available to UWP apps,
// so this only writes the exception, registers and the general info into log.txt
#include "Common/precompiled.h"
#include "ExceptionHandler.h"
#include "Common/version.h"

#include <Windows.h>

// "module+offset" for an address, empty if it isn't inside a loaded module
static std::string _DescribeAddress(uint64 address)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module) || !module)
		return {};
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(module, path, MAX_PATH);
	std::wstring name(path);
	if (const size_t slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
		name = name.substr(slash + 1);
	return fmt::format("{}+0x{:x}", std::string(name.begin(), name.end()), address - (uint64)module);
}

static LONG WINAPI _CemuUnhandledExceptionFilter(EXCEPTION_POINTERS* e)
{
	ExceptionHandler_RunCrashCallback();
	if (!CrashLog_Create())
		return EXCEPTION_CONTINUE_SEARCH;
	cemuLog_writePlainToLog(fmt::format("\nCrashlog for {}\n", BUILD_VERSION_WITH_NAME_STRING));
	if (e && e->ExceptionRecord)
	{
		const uint64 address = (uint64)e->ExceptionRecord->ExceptionAddress;
		cemuLog_writePlainToLog(fmt::format("Exception 0x{:08x} at 0x{:x} ({})\n", (uint32)e->ExceptionRecord->ExceptionCode, address, _DescribeAddress(address)));
	}
	{
		ULONG_PTR stackLow = 0, stackHigh = 0;
		GetCurrentThreadStackLimits(&stackLow, &stackHigh);
		cemuLog_writePlainToLog(fmt::format("Thread {} stack {:x}-{:x} ({} KB)\n", GetCurrentThreadId(), (uint64)stackLow, (uint64)stackHigh, (uint64)(stackHigh - stackLow) / 1024));
		// No dbghelp in UWP: list values on the stack that point into loaded modules, mostly return addresses. Offsets
		// into Cemu.exe can be resolved with the PDB from the CI build
		if (e && e->ContextRecord)
		{
			const uint64* sp = (const uint64*)e->ContextRecord->Rsp;
			int found = 0;
			for (int i = 0; i < 4096 && found < 40 && (uint64)(sp + i + 1) <= stackHigh; i++)
			{
				const std::string where = _DescribeAddress(sp[i]);
				if (!where.empty())
				{
					cemuLog_writePlainToLog(fmt::format("  stack+0x{:x}: {}\n", i * 8, where));
					found++;
				}
			}
		}
	}
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
