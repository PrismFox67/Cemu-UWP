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

#ifdef CEMU_UWP
#ifndef FILE_MAP_EXECUTE
#define FILE_MAP_EXECUTE 0x0020 // SECTION_MAP_EXECUTE_EXPLICIT
#endif
	void* AllocateExecutableMemory(size_t size, void*& writableOut)
	{
		writableOut = nullptr;
		// 1) read/write/execute memory (works where the system allows it for apps with the codeGeneration capability)
		if (void* r = VirtualAllocFromApp(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
		{
			ULONG oldProtection;
			if (VirtualProtectFromApp(r, size, PAGE_EXECUTE_READWRITE, &oldProtection))
			{
				static bool s_logged = false;
				if (!std::exchange(s_logged, true))
					cemuLog_log(LogType::Force, "MemMapper: using read/write/execute memory for generated code");
				writableOut = r;
				return r;
			}
			cemuLog_log(LogType::Force, "MemMapper: read/write/execute memory not allowed (error {}), using a double mapping", GetLastError());
			VirtualFree(r, 0, MEM_RELEASE);
		}
		// 2) one pagefile backed section mapped twice: a writable view and an executable view
		HANDLE section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, (ULONG64)size, nullptr);
		if (!section)
		{
			cemuLog_log(LogType::Force, "MemMapper: Unable to create a code section (error {}). Is the codeGeneration capability missing?", GetLastError());
			return nullptr;
		}
		void* writable = MapViewOfFileFromApp(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, size);
		void* executable = MapViewOfFileFromApp(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, size);
		CloseHandle(section); // the views keep the section alive
		if (!writable || !executable)
		{
			cemuLog_log(LogType::Force, "MemMapper: Unable to map executable memory (error {})", GetLastError());
			if (writable)
				UnmapViewOfFile(writable);
			if (executable)
				UnmapViewOfFile(executable);
			return nullptr;
		}
		static bool s_logged = false;
		if (!std::exchange(s_logged, true))
			cemuLog_log(LogType::Force, "MemMapper: using a writable and an executable view of the same memory for generated code");
		writableOut = writable;
		return executable;
	}
#else
	void* AllocateExecutableMemory(size_t size, void*& writableOut)
	{
		void* r = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		writableOut = r;
		return r;
	}
#endif

};
