#pragma once

#include "dxcompositor.h"

class DX11Compositor : public Compositor {
public:
	DX11Compositor(ID3D11Texture2D* td);

	virtual ~DX11Compositor() override;

	// Override
	virtual void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	virtual void InvokeCubemap(const vr::Texture_t* textures) override;
	virtual bool SupportsCubemap() override { return true; }

	// DX11 has the shader blit path in CopyToSwapchain that actually performs the flip.
	virtual bool SupportsShaderInvert() override { return true; }

	ID3D11Device* GetDevice() { return device; }

protected:
	void CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube);

	bool CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace);

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	// Cached shader resource view for the app's source texture, along with the texture it was created
	// from so we can tell when it needs rebuilding. Recreating this every frame is needless driver work.
	ID3D11ShaderResourceView* quad_texture_view = nullptr;
	ID3D11Texture2D* quad_texture_view_src = nullptr;

	ID3D11SamplerState* quad_sampleState = nullptr;
	ID3D11VertexShader* fs_vshader = nullptr;
	ID3D11PixelShader* fs_pshader = nullptr;

	// The sample count of the texture the current swapchain was built for. The swapchain itself is always
	// single-sampled, so this can't be recovered from createInfo.
	UINT createInfoSampleCount = 1;

	std::vector<XrSwapchainImageD3D11KHR> imagesHandles;
	std::vector<ID3D11RenderTargetView*> swapchain_rtvs;
	std::vector<ID3D11Texture2D*> resolvedMSAATextures;

	struct DxgiFormatInfo {
		/// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
		/// Both the SRGB and linear formats should be UNORM.
		DXGI_FORMAT srgb, linear, typeless;

		/// THe bits per pixel, bits per channel, and the number of channels
		int bpp, bpc, channels;
	};

	/**
	 * Gets information about a given format into the output variable. Returns true if the texture was
	 * found, if not it returns false and leaves out in an undefined state.
	 */
	static bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo& out);
};
