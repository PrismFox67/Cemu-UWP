cmake_minimum_required(VERSION 3.21.1)

option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)
option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow Cemu to be run in portable mode" ON)

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true) # needs to be set before vcpkg

execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

if (ENABLE_VCPKG)
	# check if vcpkg is shallow and unshallow it if necessary
	execute_process(
	COMMAND git rev-parse --is-shallow-repository
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/vcpkg
	OUTPUT_VARIABLE is_vcpkg_shallow
	OUTPUT_STRIP_TRAILING_WHITESPACE
    )

	if(is_vcpkg_shallow STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(
			COMMAND git fetch --unshallow
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/vcpkg"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
		)
	endif()

	if(UNIX AND NOT APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_linux;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	elseif(APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_mac;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	else()
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_win;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	endif()
	set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake"
		CACHE STRING "Vcpkg toolchain file")
	# Set this so that all the various find_package() calls don't need an explicit
	# CONFIG option
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	if (CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
		# UWP (Xbox): static libraries with the DLL runtime, see dependencies/vcpkg_triplets
		set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_triplets" CACHE STRING "")
		set(VCPKG_TARGET_TRIPLET "x64-uwp-static-md" CACHE STRING "")
	elseif (WIN32)
		set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
	endif()
endif()

project(Cemu VERSION 2.0.0)

if (CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	set(CEMU_UWP ON)
	add_compile_definitions(CEMU_UWP)
	# CMAKE_SYSTEM_PROCESSOR is empty when CMAKE_SYSTEM_NAME is set explicitly, the architecture checks need it
	if (NOT CMAKE_SYSTEM_PROCESSOR)
		set(CMAKE_SYSTEM_PROCESSOR "AMD64")
	endif()
	# UWP apps use the DLL runtime (the VCLibs framework package)
	set(CEMU_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	# desktop only components
	set(ENABLE_WXWIDGETS OFF)
	set(ENABLE_HOST_FRONTEND ON)
	set(ENABLE_OPENGL OFF)
	set(ENABLE_VULKAN OFF)
	set(ENABLE_SDL OFF)
	set(ENABLE_LIBUSB OFF)
	set(ENABLE_HIDAPI OFF)
	set(ENABLE_CUBEB OFF)
	set(ENABLE_DIRECTINPUT OFF)
	set(ENABLE_DIRECTAUDIO OFF)
	set(ENABLE_DISCORD_RPC OFF)
	set(ENABLE_D3D12 ON)
else()
	set(CEMU_UWP OFF)
	set(CEMU_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# enable link time optimization for release builds
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if (MSVC)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	# floating point model: precise, fiber safe optimizations
	add_compile_options(/EHsc /fp:precise)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# Speeds up static linking (especially helpful in incremental compilation)
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
  	else()
	  add_compile_options(/GT)
	endif()
	# enable additional optimization flags for release builds
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>) # enable intrinsic functions
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>) # favor speed
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
endif()

if (APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
endif()

if (UNIX AND NOT APPLE)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

if (APPLE)
    set(ENABLE_METAL_DEFAULT ON)
	set(ENABLE_OPENGL_DEFAULT OFF)
else()
    set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT ON)
endif()

option(ENABLE_OPENGL "Enables the OpenGL backend" ${ENABLE_OPENGL_DEFAULT})
option(ENABLE_VULKAN "Enables the Vulkan backend" ON)
option(ENABLE_METAL "Enables the Metal backend" ${ENABLE_METAL_DEFAULT})
if (WIN32)
	# shaders go through SPIRV-Cross (vcpkg) and d3dcompiler_47 (part of Windows), see src/Cafe/HW/Latte/Renderer/D3D12/README.md
	option(ENABLE_D3D12 "Enables the Direct3D 12 backend" ON)
	option(ENABLE_D3D12_SPIRV_TO_DXIL "D3D12: also build the optional Mesa spirv_to_dxil shader path" OFF)
endif()
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)

if (ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "Metal backend is only supported on Apple platforms")
endif()

# input backends
if (WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	if (ENABLE_DIRECTINPUT)
		add_compile_definitions(HAS_DIRECTINPUT)
	endif()
endif()

option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)

if (ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
if (WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ON)
	option(ENABLE_XAUDIO "Enables the xaudio backend" ON)
endif()
option(ENABLE_CUBEB "Enabled cubeb backend" ON)

option(ENABLE_WXWIDGETS "Build with wxWidgets UI" ON)
# ImGui frontend without wxWidgets, rendering with D3D12. It is the UI of the UWP/Xbox build and can be built as a
# Win32 executable to test it on a PC. Mutually exclusive with ENABLE_WXWIDGETS (both implement WindowSystem)
option(ENABLE_HOST_FRONTEND "Build the wxWidgets-free D3D12 frontend (UWP/Xbox UI)" OFF)
if (ENABLE_HOST_FRONTEND)
	if (ENABLE_WXWIDGETS)
		message(FATAL_ERROR "ENABLE_HOST_FRONTEND requires ENABLE_WXWIDGETS=OFF")
	endif()
	if (NOT ENABLE_D3D12)
		message(FATAL_ERROR "ENABLE_HOST_FRONTEND requires ENABLE_D3D12")
	endif()
	add_compile_definitions(CEMU_HOST_FRONTEND)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(RapidJSON REQUIRED)
if (CEMU_UWP)
	# vcpkg's boost-filesystem does not support UWP; Cemu itself only needs it indirectly on desktop (nowide)
	find_package(Boost COMPONENTS program_options nowide REQUIRED)
else()
	find_package(Boost COMPONENTS program_options filesystem nowide REQUIRED)
endif()
find_package(libzip REQUIRED)
find_package(glslang REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # MODULE so that zstd::zstd is available
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if(ENABLE_SDL)
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	add_compile_definitions(HAS_SDL)
endif()

# glslang versions older than 11.11.0 define targets without a namespace
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if (UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	if (ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)

		ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${WAYLAND_PROTOCOL_SRCS})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()
	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()

endif()

if (ENABLE_VULKAN)
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_D3D12)
	if (NOT WIN32)
		message(FATAL_ERROR "The D3D12 backend is only supported on Windows")
	endif()
	# Shaders: Vulkan GLSL -> SPIR-V (glslang) -> HLSL (SPIRV-Cross) -> DXBC (d3dcompiler_47, part of Windows)
	find_package(spirv_cross_core CONFIG REQUIRED)
	find_package(spirv_cross_glsl CONFIG REQUIRED)
	find_package(spirv_cross_hlsl CONFIG REQUIRED)
	# Optional alternative shader path through Mesa's spirv_to_dxil (meson -Dspirv-to-dxil=true). Off by default
	if (ENABLE_D3D12_SPIRV_TO_DXIL)
		find_path(SPIRV_TO_DXIL_INCLUDE_DIR NAMES spirv_to_dxil.h
			HINTS ${SPIRV_TO_DXIL_ROOT} $ENV{SPIRV_TO_DXIL_ROOT}
			PATH_SUFFIXES include include/spirv_to_dxil src/microsoft/spirv_to_dxil)
		find_library(SPIRV_TO_DXIL_LIBRARY NAMES spirv_to_dxil libspirv_to_dxil
			HINTS ${SPIRV_TO_DXIL_ROOT} $ENV{SPIRV_TO_DXIL_ROOT}
			PATH_SUFFIXES lib lib64 bin)
		if (NOT SPIRV_TO_DXIL_INCLUDE_DIR OR NOT SPIRV_TO_DXIL_LIBRARY)
			message(FATAL_ERROR "ENABLE_D3D12_SPIRV_TO_DXIL requires spirv_to_dxil. Set SPIRV_TO_DXIL_ROOT to the Mesa install prefix")
		endif()
	endif()
	add_compile_definitions(ENABLE_D3D12)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if(UNIX AND NOT APPLE)
	if(ENABLE_FERAL_GAMEMODE)
		add_compile_definitions(ENABLE_FERAL_GAMEMODE)
		add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
		target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
	endif()
endif()

if (ENABLE_WXWIDGETS)
	find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core gl propgrid xrc)
endif()

if (ENABLE_CUBEB)
	if (NOT ENABLE_VCPKG)
	    find_package(cubeb)
	endif()
	if (NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME}")
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if (NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
	if (TARGET zarchive)
		set_property(TARGET zarchive PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME}")
	endif()
endif()

add_subdirectory(src)
