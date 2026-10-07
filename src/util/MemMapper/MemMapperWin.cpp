#include "util/MemMapper/MemMapper.h"

#include <Windows.h>

namespace MemMapper
{
	const size_t sPageSize{ []()
		{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		return (size_t)si.dwPageSize;
	}()
	};

	size_t GetPageSize()
	{
		return sPageSize;
	}

	DWORD GetPageProtection(PAGE_PERMISSION permissionFlags)
	{
		DWORD p = 0;
		if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PAGE_EXECUTE_READWRITE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PAGE_READWRITE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PAGE_READONLY;
		else
			cemu_assert_unimplemented();
		return p;
	}

#ifdef CEMU_UWP
	// UWP apps allocate through the *FromApp variants, which reject executable protections. Executable memory (the PPC
	// recompiler's code cache) is allocated read/write and then switched to read/write/execute, which requires the
	// "codeGeneration" capability in the app manifest
	static void* _VirtualAlloc(void* baseAddr, size_t size, DWORD allocationType, PAGE_PERMISSION permissionFlags)
	{
		const bool executable = HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE);
		const DWORD protection = executable ? PAGE_READWRITE : GetPageProtection(permissionFlags);
		void* r = VirtualAllocFromApp(baseAddr, size, allocationType, protection);
		if (r && executable && (allocationType & MEM_COMMIT))
		{
			ULONG oldProtection;
			if (!VirtualProtectFromApp(r, size, PAGE_EXECUTE_READWRITE, &oldProtection))
				cemuLog_log(LogType::Force, "MemMapper: Unable to make memory executable (error {}). Is the codeGeneration capability missing?", GetLastError());
		}
		return r;
	}
#else
	static void* _VirtualAlloc(void* baseAddr, size_t size, DWORD allocationType, PAGE_PERMISSION permissionFlags)
	{
		return VirtualAlloc(baseAddr, size, allocationType, GetPageProtection(permissionFlags));
	}
#endif

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		void* r = _VirtualAlloc(baseAddr, size, MEM_RESERVE, permissionFlags);
		return r;
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		VirtualFree(baseAddr, size, MEM_RELEASE);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		void* r;
		if(fromReservation)
			r = _VirtualAlloc(baseAddr, size, MEM_COMMIT, permissionFlags);
		else
			r = _VirtualAlloc(baseAddr, size, MEM_RESERVE | MEM_COMMIT, permissionFlags);
		return r;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if(fromReservation)
			VirtualFree(baseAddr, size, MEM_DECOMMIT);
		else
			VirtualFree(baseAddr, size, MEM_RELEASE);
	}

};
