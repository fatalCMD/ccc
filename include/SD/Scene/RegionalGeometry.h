#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace SD::Scene::Regional
{
	struct Vertex
	{
		float blinkLeft{}, blinkRight{};
		std::array<std::array<float, 3>, 7> delta{};
	};
	static_assert(sizeof(Vertex) == 92);
	using Position = std::array<float, 4>;
	struct Undo { Position before{}, after{}; bool owned{ false }; };

	inline void Restore(std::span<Position> vertices, std::span<Undo> undo)
	{
		for (std::size_t i = 0; i < std::min(vertices.size(), undo.size()); ++i) {
			auto& u = undo[i];
			auto& p = vertices[i];
			if (u.owned && p[0] == u.after[0] && p[1] == u.after[1] && p[2] == u.after[2]) {
				std::copy_n(u.before.begin(), 3, p.begin());
			}
			u.owned = false;
		}
	}

	inline float Apply(std::span<Position> vertices, std::span<const Vertex> asset,
		std::span<Undo> undo, const std::array<float, 8>& weights, float leftBlink, float rightBlink)
	{
		if (vertices.size() != asset.size() || undo.size() != asset.size()) return 0;
		float peak = 0;
		for (std::size_t i = 0; i < asset.size(); ++i) {
			const auto& a = asset[i];
			std::array<float, 3> delta{};
			const float eye = 1 - std::clamp(std::max(a.blinkLeft * leftBlink, a.blinkRight * rightBlink), 0.0f, 1.0f);
			for (std::size_t m = 0; m < a.delta.size(); ++m) {
				const float weight = std::isfinite(weights[m + 1]) ? std::clamp(weights[m + 1], 0.0f, .85f) : 0;
				for (std::size_t c = 0; c < 3; ++c) delta[c] += a.delta[m][c] * weight * eye;
			}
			if (delta == std::array<float, 3>{}) continue;
			auto& p = vertices[i];
			if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
			undo[i].before = p;
			for (std::size_t c = 0; c < 3; ++c) p[c] += std::clamp(delta[c], -.75f, .75f);
			undo[i].after = p;
			undo[i].owned = true;
			peak = std::max(peak, std::sqrt(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]));
		}
		return peak;
	}
}
