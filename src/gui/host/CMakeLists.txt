# wxWidgets-free frontend: ImGui launcher + D3D12 renderer. UI of the UWP/Xbox build, also buildable as a Win32
# executable for testing (ENABLE_HOST_FRONTEND with ENABLE_WXWIDGETS=OFF)

add_library(CemuHostFrontend
	HostApp.cpp
	HostApp.h
	HostControllers.cpp
	HostControllers.h
	HostGamepad.cpp
	HostGamepad.h
	HostGraphicPacks.cpp
	HostGraphicPacks.h
	HostIcons.cpp
	HostIcons.h
	HostMusic.cpp
	HostMusic.h
	HostMusicDecoder.cpp
	HostMusicDecoder.h
	HostPlatform.h
	HostTitleInstaller.cpp
	HostTitleInstaller.h
	HostTheme.cpp
	HostTheme.h
	HostTools.cpp
	HostTools.h
	HostTranslation.cpp
	HostTranslation.h
	HostUI.cpp
	HostUI.h
	HostWindowSystem.cpp
	LauncherRenderer.cpp
	LauncherRenderer.h
	${CMAKE_SOURCE_DIR}/dependencies/imgui/backends/imgui_impl_dx12.cpp
	${CMAKE_SOURCE_DIR}/dependencies/imgui/backends/imgui_impl_dx12.h
)

if (CEMU_UWP)
	target_sources(CemuHostFrontend PRIVATE
		uwp/HostMainUWP.cpp
	)
	# C++/WinRT headers don't mix with the shared precompiled header, the file includes it itself
	set_source_files_properties(uwp/HostMainUWP.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
	target_link_libraries(CemuHostFrontend PRIVATE xinputuap WindowsApp)
else()
	target_sources(CemuHostFrontend PRIVATE
		win32/HostMainWin32.cpp
	)
	target_link_libraries(CemuHostFrontend PRIVATE xinput)
endif()

set_property(TARGET CemuHostFrontend PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME}")

cemu_use_precompiled_header(CemuHostFrontend)
# the vendored C decoders (thirdparty/) are compiled in this file and don't build with the shared precompiled header
set_source_files_properties(HostMusicDecoder.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# with link-time code generation the linker reports "LNK1103: debugging information corrupt" for HostUI.obj as soon as
# it calls into HostMusic (MSVC 14.5, /Z7): the music code is built without /GL
set_source_files_properties(HostMusic.cpp HostMusicDecoder.cpp PROPERTIES COMPILE_OPTIONS "/GL-")

target_include_directories(CemuHostFrontend PRIVATE
	"${CMAKE_SOURCE_DIR}/src/gui"
	"${CMAKE_SOURCE_DIR}/dependencies/imgui"
	${RAPIDJSON_INCLUDE_DIRS}
)

target_link_libraries(CemuHostFrontend PRIVATE
	CemuAudio
	CemuCafe
	CemuCommon
	CemuConfig
	CemuInput
	CemuUtil
	CemuResource
	imguiImpl
	libzip::zip
	CURL::libcurl
	d3d12
	dxgi
	d3dcompiler
)
