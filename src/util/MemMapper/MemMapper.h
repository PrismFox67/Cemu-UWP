#pragma once

namespace MemMapper
{
	enum class PAGE_PERMISSION : uint32
	{
		P_READ = (0x01),
		P_WRITE = (0x02),
		P_EXECUTE = (0x04),
		// combined
		P_NONE = 0,
		P_RW = (0x03),
		P_RWX = (0x07)
	};
	DEFINE_ENUM_FLAG_OPERATORS(PAGE_PERMISSION);

	size_t GetPageSize();

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags);
	void FreeReservation(void* baseAddr, size_t size);

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation = false);
	void FreeMemory(void* baseAddr, size_t size, bool fromReservation = false);

	// Memory for generated code. The code runs at the returned address and is written through writableOut. Both are the
	// same address unless the platform refuses memory that is writable and executable at once (UWP), in which case the
	// memory is mapped twice. Returns nullptr if no executable memory can be allocated
	void* AllocateExecutableMemory(size_t size, void*& writableOut);
};