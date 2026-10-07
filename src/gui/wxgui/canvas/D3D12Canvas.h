#pragma once

#include "wxgui/canvas/IRenderCanvas.h"

#include <wx/frame.h>

// Desktop (Win32) host for the D3D12 renderer. UWP hosts don't use wxWidgets and pass a CoreWindow or
// SwapChainPanel to D3D12Renderer::InitializeSurface through WindowSystem instead.
class D3D12Canvas : public IRenderCanvas, public wxWindow
{
public:
	D3D12Canvas(wxWindow* parent, const wxSize& size, bool is_main_window);
	~D3D12Canvas();

private:
	void OnPaint(wxPaintEvent& event);
	void OnResize(wxSizeEvent& event);
};
