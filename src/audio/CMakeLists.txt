add_library(CemuAudio
	IAudioAPI.cpp
	IAudioAPI.h
	IAudioInputAPI.cpp
	IAudioInputAPI.h
)

set_property(TARGET CemuAudio PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME}")

cemu_use_precompiled_header(CemuAudio)

if(WIN32)
	target_sources(CemuAudio PRIVATE
		XAudio2API.cpp
		XAudio2API.h
	)
	if (NOT CEMU_UWP)
		target_sources(CemuAudio PRIVATE
			DirectSoundAPI.cpp
			DirectSoundAPI.h
			XAudio27API.cpp
			XAudio27API.h
		)
	endif()
endif()

if(ENABLE_CUBEB)
	target_sources(CemuAudio PRIVATE
		CubebAPI.cpp
		CubebAPI.h
		CubebInputAPI.cpp
		CubebInputAPI.h
	)
	#add_compile_definitions(HAS_CUBEB)
endif()

target_include_directories(CemuAudio PUBLIC "../")

target_link_libraries(CemuAudio PRIVATE
	CemuCommon
	CemuGui
)

if(ENABLE_CUBEB)
	# PUBLIC because cubeb.h/cubeb.h is included in CubebAPI.h
	target_link_libraries(CemuAudio PUBLIC cubeb::cubeb)
endif()
