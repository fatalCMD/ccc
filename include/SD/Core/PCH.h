#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace logger = SKSE::log;
using namespace std::literals;

namespace SD
{
	using Microsoft::WRL::ComPtr;
}
