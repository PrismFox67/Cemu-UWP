add_library(CemuGui INTERFACE)
target_include_directories(CemuGui INTERFACE "interface")

if(ENABLE_WXWIDGETS)
    add_subdirectory(wxgui)
    target_link_libraries(CemuGui INTERFACE CemuWxGui)
endif()

if(ENABLE_HOST_FRONTEND)
    add_subdirectory(host)
    target_link_libraries(CemuGui INTERFACE CemuHostFrontend)
endif()
