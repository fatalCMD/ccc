#include "SD/Render/Letterbox.h"

#include "SD/Core/Logging.h"

#include <chrono>

namespace SD::Render
{
	namespace
	{
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		// IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, Present 8.
		constexpr std::size_t kPresentIndex = 8;

		// Fraction of screen height each bar occupies at full extension. 2.35:1 on
		// a 16:9 frame works out near this; it reads as scope without eating the
		// subtitles.
		//
		// Atomic because the present hook reads it on the render thread while the
		// director writes it from the game thread at the start of a conversation.
		// A torn float here would be a one-frame wrong bar height, but the fix is
		// free so there is no reason to accept even that.
		std::atomic<float> barFraction{ 0.115f };
		constexpr float    kEaseSeconds = 0.32f;

		std::atomic<PresentFn> originalPresent{ nullptr };
		std::atomic_bool       installed{ false };
		std::atomic_bool       enabled{ true };
		std::atomic_bool       wantVisible{ false };

		// "Off the screen now, not eased off." See Letterbox::Retract.
		std::atomic_bool       snapClosed{ false };

		// A menu owns the screen, as MenuWatch understands it rather than as the
		// pause counter does. See Letterbox::SetScreenTaken.
		std::atomic_bool       screenTaken{ false };

		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader>  pixelShader;
		ComPtr<ID3D11InputLayout>  inputLayout;
		ComPtr<ID3D11Buffer>       vertexBuffer;
		ComPtr<ID3D11BlendState>   blendState;
		ComPtr<ID3D11DepthStencilState> depthState;
		ComPtr<ID3D11RasterizerState>   rasterState;
		ComPtr<ID3D11RenderTargetView>  renderTarget;
		bool                            resourcesReady{ false };

		float                                 extension{ 0.0f };
		std::chrono::steady_clock::time_point lastDraw{};
		bool                                  haveLastDraw{ false };

		Log::OnceFlag firstDrawReported;
		Log::OnceFlag menuRetractReported;

		constexpr char kShaderSource[] = R"(
struct VSIn  { float2 pos : POSITION; };
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0f, 1.0f); return o; }
float4 PSMain(VSOut i) : SV_TARGET { return float4(0.0f, 0.0f, 0.0f, 1.0f); }
)";

		void Disable(std::string_view a_reason)
		{
			enabled.store(false, std::memory_order_release);
			Log::Error(Log::Category::kRender, "Letterbox disabled for this session: {}"sv, a_reason);
		}

		[[nodiscard]] bool CreateResources(ID3D11Device* a_device, IDXGISwapChain* a_swapChain)
		{
			ComPtr<ID3DBlob> vsBlob, psBlob, errors;

			if (FAILED(::D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
					"VSMain", "vs_5_0", 0, 0, &vsBlob, &errors))) {
				Disable("vertex shader compilation failed"sv);
				return false;
			}
			if (FAILED(::D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
					"PSMain", "ps_5_0", 0, 0, &psBlob, &errors))) {
				Disable("pixel shader compilation failed"sv);
				return false;
			}

			if (FAILED(a_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader)) ||
				FAILED(a_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader))) {
				Disable("shader creation failed"sv);
				return false;
			}

			const D3D11_INPUT_ELEMENT_DESC element{
				"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
			};
			if (FAILED(a_device->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout))) {
				Disable("input layout creation failed"sv);
				return false;
			}

			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = sizeof(float) * 2 * 12;  // two quads, six vertices each
			bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
			bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(a_device->CreateBuffer(&bufferDesc, nullptr, &vertexBuffer))) {
				Disable("vertex buffer creation failed"sv);
				return false;
			}

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(a_device->CreateBlendState(&blendDesc, &blendState))) {
				Disable("blend state creation failed"sv);
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = FALSE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (FAILED(a_device->CreateDepthStencilState(&depthDesc, &depthState))) {
				Disable("depth state creation failed"sv);
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc{};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.DepthClipEnable = FALSE;
			if (FAILED(a_device->CreateRasterizerState(&rasterDesc, &rasterState))) {
				Disable("rasterizer state creation failed"sv);
				return false;
			}

			ComPtr<ID3D11Texture2D> backBuffer;
			if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
				FAILED(a_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget))) {
				Disable("render target view creation failed"sv);
				return false;
			}

			resourcesReady = true;
			Log::Info(Log::Category::kRender, "Letterbox resources created."sv);
			return true;
		}

		void WriteBars(ID3D11DeviceContext* a_context, float a_height)
		{
			// Clip space: y = 1 at the top, -1 at the bottom. A bar of a_height in
			// screen fractions is 2 * a_height tall here.
			const float h = a_height * 2.0f;
			const float top = 1.0f;
			const float topInner = 1.0f - h;
			const float bottom = -1.0f;
			const float bottomInner = -1.0f + h;

			const std::array<float, 24> vertices{
				-1.0f, top,   1.0f, top,   -1.0f, topInner,
				 1.0f, top,   1.0f, topInner, -1.0f, topInner,

				-1.0f, bottomInner, 1.0f, bottomInner, -1.0f, bottom,
				 1.0f, bottomInner, 1.0f, bottom,      -1.0f, bottom
			};

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, vertices.data(), sizeof(vertices));
				a_context->Unmap(vertexBuffer.Get(), 0);
			}
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			auto* manager = RE::BSGraphics::Renderer::GetSingleton();
			if (!manager) {
				return;
			}
			auto& runtime = manager->GetRuntimeData();
			if (!runtime.forwarder || !runtime.context) {
				return;
			}

			auto* device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(runtime.context);

			if (!resourcesReady && !CreateResources(device, a_swapChain)) {
				return;
			}

			// Ease toward the target so the bars slide rather than pop.
			const auto now = std::chrono::steady_clock::now();
			float      delta = 1.0f / 60.0f;
			if (haveLastDraw) {
				delta = std::clamp(std::chrono::duration<float>(now - lastDraw).count(), 0.0f, 0.25f);
			}
			lastDraw = now;
			haveLastDraw = true;

			// Retract instantly, from here, whenever a menu owns the screen.
			//
			// This has to be decided HERE and not in the director's tick, because
			// that tick rides PlayerCharacter::Update and stops while a pausing menu
			// is up. Present does not. So opening barter, an inventory or the map
			// froze the director mid-conversation with the bars still extended, and
			// they sat across the merchant's inventory until the menu closed and the
			// tick resumed.
			//
			// WHAT THIS NO LONGER ASKS IS RE::UI::GameIsPaused(), and that read is
			// the whole of a reported bug.
			//
			// numPausesGame is not "a menu owns the screen". It counts the CONSOLE,
			// which this mod deliberately treats as an overlay over a scene that has
			// not moved. It counts any overlay a mod puts up that freezes time. And
			// it counts SKSE Menu Framework's own settings panel, whose
			// FreezeTimeOnMenu option is shipped as true by more than one mod that
			// bundles the framework.
			//
			// That last one is not theoretical. It meant the bars were forced off
			// the screen for exactly as long as the panel that configures them was
			// open: tick Black Bars, drag Bar Height, watch nothing happen, conclude
			// the switch is broken. Both settings were applying live the whole time
			// and neither could be seen. Reported as "black bar cannot be brought up
			// no matter what I set".
			//
			// The two flags below are the narrower question, asked of the two places
			// that actually know the answer. `screenTaken` is MenuWatch's level
			// answer — it carries the Console exemption and it catches CraftingMenu,
			// which owns the screen without pausing anything. `snapClosed` is the
			// director's edge: take the bars off NOW rather than easing them, on the
			// frame a menu is about to draw over them.
			const bool taken = snapClosed.load(std::memory_order_acquire) ||
				screenTaken.load(std::memory_order_acquire);
			if (taken) {
				if (extension > 0.0f && menuRetractReported.Take()) {
					Log::Info(Log::Category::kRender,
						"Menu took the screen; bars retracted from the present hook."sv);
				}
				extension = 0.0f;
				return;
			}

			const float target = wantVisible.load(std::memory_order_acquire) ? 1.0f : 0.0f;
			const float step = delta / kEaseSeconds;
			extension += std::clamp(target - extension, -step, step);

			if (extension <= 0.001f) {
				return;  // fully retracted; touch nothing
			}

			// Back up everything about to be overwritten. Drawing inside Present
			// without restoring corrupts whatever the game renders next frame, and
			// the symptom is a stretched or missing UI rather than anything that
			// points here.
			ComPtr<ID3D11RenderTargetView> savedRTV;
			ComPtr<ID3D11DepthStencilView> savedDSV;
			context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

			ComPtr<ID3D11BlendState> savedBlend;
			float                    savedBlendFactor[4]{};
			UINT                     savedSampleMask = 0;
			context->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);

			ComPtr<ID3D11DepthStencilState> savedDepth;
			UINT                            savedStencilRef = 0;
			context->OMGetDepthStencilState(&savedDepth, &savedStencilRef);

			ComPtr<ID3D11RasterizerState> savedRaster;
			context->RSGetState(&savedRaster);

			UINT               savedViewportCount = 1;
			D3D11_VIEWPORT     savedViewport{};
			context->RSGetViewports(&savedViewportCount, &savedViewport);

			ComPtr<ID3D11InputLayout> savedLayout;
			context->IAGetInputLayout(&savedLayout);
			D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
			context->IAGetPrimitiveTopology(&savedTopology);

			ComPtr<ID3D11Buffer> savedVB;
			UINT                 savedStride = 0;
			UINT                 savedOffset = 0;
			context->IAGetVertexBuffers(0, 1, &savedVB, &savedStride, &savedOffset);

			ComPtr<ID3D11VertexShader> savedVS;
			ComPtr<ID3D11PixelShader>  savedPS;
			context->VSGetShader(&savedVS, nullptr, nullptr);
			context->PSGetShader(&savedPS, nullptr, nullptr);

			WriteBars(context, barFraction.load(std::memory_order_relaxed) * extension);

			const UINT stride = sizeof(float) * 2;
			const UINT offset = 0;
			ID3D11RenderTargetView* rtv = renderTarget.Get();
			const float             blendFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };

			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->OMSetBlendState(blendState.Get(), blendFactor, 0xFFFFFFFF);
			context->OMSetDepthStencilState(depthState.Get(), 0);
			context->RSSetState(rasterState.Get());
			context->IASetInputLayout(inputLayout.Get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
			context->VSSetShader(vertexShader.Get(), nullptr, 0);
			context->PSSetShader(pixelShader.Get(), nullptr, 0);
			context->Draw(12, 0);

			// Restore, in reverse.
			context->VSSetShader(savedVS.Get(), nullptr, 0);
			context->PSSetShader(savedPS.Get(), nullptr, 0);
			context->IASetVertexBuffers(0, 1, savedVB.GetAddressOf(), &savedStride, &savedOffset);
			context->IASetPrimitiveTopology(savedTopology);
			context->IASetInputLayout(savedLayout.Get());
			if (savedViewportCount > 0) {
				context->RSSetViewports(1, &savedViewport);
			}
			context->RSSetState(savedRaster.Get());
			context->OMSetDepthStencilState(savedDepth.Get(), savedStencilRef);
			context->OMSetBlendState(savedBlend.Get(), savedBlendFactor, savedSampleMask);
			context->OMSetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.Get());

			if (firstDrawReported.Take()) {
				Log::Info(Log::Category::kRender, "Letterbox drawing; bar height {:.1f}% of frame."sv,
					barFraction.load(std::memory_order_relaxed) * 100.0f);
			}
		}

		HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			const auto original = originalPresent.load(std::memory_order_acquire);

			if (enabled.load(std::memory_order_acquire)) {
				try {
					Draw(a_swapChain);
				} catch (...) {
					Disable("draw threw an exception"sv);
				}
			}

			return original ? original(a_swapChain, a_sync, a_flags) : E_FAIL;
		}
	}

	void Letterbox::Install()
	{
		if (installed.load(std::memory_order_relaxed)) {
			return;
		}

		auto* manager = RE::BSGraphics::Renderer::GetSingleton();
		if (!manager) {
			Log::Error(Log::Category::kRender, "Renderer unavailable; no letterbox."sv);
			return;
		}

		auto& runtime = manager->GetRuntimeData();
		auto* swapChain = reinterpret_cast<IDXGISwapChain*>(runtime.renderWindows[0].swapChain);
		if (!swapChain) {
			Log::Error(Log::Category::kRender, "Swap chain unavailable; no letterbox."sv);
			return;
		}

		auto** vtable = *reinterpret_cast<void***>(swapChain);
		if (!vtable) {
			Log::Error(Log::Category::kRender, "Swap chain vtable unreadable; no letterbox."sv);
			return;
		}

		void** slot = &vtable[kPresentIndex];
		DWORD  protection = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) {
			Log::Error(Log::Category::kRender, "Could not unprotect the Present slot; no letterbox."sv);
			return;
		}

		originalPresent.store(reinterpret_cast<PresentFn>(*slot), std::memory_order_release);
		*slot = reinterpret_cast<void*>(&DetourPresent);

		DWORD ignored = 0;
		::VirtualProtect(slot, sizeof(void*), protection, &ignored);

		installed.store(true, std::memory_order_release);
		Log::Info(Log::Category::kRender, "Present hook installed for the letterbox."sv);
	}

	void Letterbox::Shutdown()
	{
		// The bars are retracted rather than the hook removed. A render thread can
		// already be inside the detour, and unhooking underneath it is how an
		// overlay takes the game down on exit.
		wantVisible.store(false, std::memory_order_release);
		enabled.store(false, std::memory_order_release);
	}

	void Letterbox::SetBarFraction(float a_fraction)
	{
		// Capped well below a half — two bars at 0.5 each would close the frame
		// entirely, and a setting that can black out the screen is a bug report
		// waiting to happen.
		barFraction.store(std::clamp(a_fraction, 0.0f, 0.30f), std::memory_order_relaxed);
	}

	void Letterbox::SetVisible(bool a_visible)
	{
		// Asking for the bars clears the snap. A conversation resuming after a
		// trade eases them back in like any other; the snap is only ever a way of
		// getting them off the screen faster than the ease could.
		if (a_visible) {
			snapClosed.store(false, std::memory_order_release);
		}
		wantVisible.store(a_visible, std::memory_order_release);
	}

	void Letterbox::Retract()
	{
		wantVisible.store(false, std::memory_order_release);
		snapClosed.store(true, std::memory_order_release);
	}

	void Letterbox::SetScreenTaken(bool a_taken)
	{
		screenTaken.store(a_taken, std::memory_order_release);
	}

	bool Letterbox::Installed() noexcept
	{
		return installed.load(std::memory_order_acquire);
	}
}
