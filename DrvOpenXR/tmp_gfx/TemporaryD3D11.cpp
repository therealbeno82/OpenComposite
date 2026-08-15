//
// Created by ZNix on 8/02/2021.
//

#if defined(SUPPORT_DX11)

#include "TemporaryD3D11.h"

static IDXGIAdapter1* d3d_get_adapter(const LUID& adapter_luid)
{
	// Turn the LUID into a specific graphics device adapter
	IDXGIAdapter1* final_adapter = nullptr;
	IDXGIAdapter1* curr_adapter = nullptr;
	IDXGIFactory1* dxgi_factory = nullptr;
	DXGI_ADAPTER_DESC1 adapter_desc;

	OOVR_LOG("[startup]   d3d_get_adapter: calling CreateDXGIFactory1...");
	OOVR_FAILED_DX_ABORT(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&dxgi_factory)));
	OOVR_LOG("[startup]   d3d_get_adapter: CreateDXGIFactory1 OK, enumerating adapters...");

	int curr = 0;
	while (dxgi_factory->EnumAdapters1(curr++, &curr_adapter) == S_OK) {
		curr_adapter->GetDesc1(&adapter_desc);
		OOVR_LOGF("[startup]   d3d_get_adapter: adapter %d = '%ws' (LUID %08x:%08x)",
		    curr - 1, adapter_desc.Description,
		    (unsigned)adapter_desc.AdapterLuid.HighPart, (unsigned)adapter_desc.AdapterLuid.LowPart);

		// Note: sizeof(adapter_luid), not sizeof(&adapter_luid) - the latter is the size of a pointer,
		// which only happens to equal sizeof(LUID) on 64-bit. In the 32-bit builds it compared just half
		// the LUID and could pick the wrong GPU on a multi-adapter machine.
		if (memcmp(&adapter_desc.AdapterLuid, &adapter_luid, sizeof(adapter_luid)) == 0) {
			final_adapter = curr_adapter;
			break;
		}
		curr_adapter->Release();
		curr_adapter = nullptr;
	}
	dxgi_factory->Release();
	return final_adapter;
}

TemporaryD3D11::TemporaryD3D11()
{
	// The spec requires that we call this first, and use it to get the correct things
	XrGraphicsRequirementsD3D11KHR graphicsRequirements{};
	graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
	XrResult res = xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements);
	OOVR_FAILED_XR_ABORT(res);

	OOVR_LOG("[startup] TemporaryD3D11: looking up DXGI adapter for the runtime's LUID...");
	IDXGIAdapter1* adapter = d3d_get_adapter(graphicsRequirements.adapterLuid);
	if (!adapter) {
		OOVR_ABORT("Could not find the DXGI adapter that the OpenXR runtime requires.\n"
		           "On a multi-GPU system, check that your headset is connected to the GPU your runtime is using.");
	}

	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

	if (graphicsRequirements.minFeatureLevel > D3D_FEATURE_LEVEL_11_0)
		featureLevels[0] = graphicsRequirements.minFeatureLevel;

	// Such a horrid hack - of all the ugly things we do in OpenComposite, this has to be one of the worst.
	OOVR_LOG("[startup] TemporaryD3D11: calling D3D11CreateDevice...");
	HRESULT createDeviceRes = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
	    featureLevels, _countof(featureLevels),
	    D3D11_SDK_VERSION, &device, nullptr, nullptr);
	OOVR_LOGF("[startup] TemporaryD3D11: D3D11CreateDevice returned 0x%08x", (unsigned)createDeviceRes);

	OOVR_FAILED_DX_ABORT(createDeviceRes);

	d3dInfo = XrGraphicsBindingD3D11KHR{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	d3dInfo.device = device;
	adapter->Release();
	OOVR_LOG("[startup] TemporaryD3D11: temporary D3D11 device created OK");
}

TemporaryD3D11::~TemporaryD3D11()
{
	device->Release();
}

#endif
