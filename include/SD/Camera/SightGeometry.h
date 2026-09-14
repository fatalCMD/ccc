#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace SD::Camera
{
	enum class SightState : std::uint8_t
	{
		kUnknown,
		kClear,
		kBlocked
	};

	// Engine-independent geometry shared by the runtime and standalone tests.
	namespace SightGeometry
	{
		struct Point
		{
			float x{}, y{}, z{};
		};

		[[nodiscard]] inline bool Finite(Point a_p)
		{
			return std::isfinite(a_p.x) && std::isfinite(a_p.y) && std::isfinite(a_p.z);
		}

		[[nodiscard]] inline Point Add(Point a_a, Point a_b) { return { a_a.x + a_b.x, a_a.y + a_b.y, a_a.z + a_b.z }; }
		[[nodiscard]] inline Point Sub(Point a_a, Point a_b) { return { a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z }; }
		[[nodiscard]] inline Point Mul(Point a_a, float a_s) { return { a_a.x * a_s, a_a.y * a_s, a_a.z * a_s }; }
		[[nodiscard]] inline float Dot(Point a_a, Point a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
		[[nodiscard]] inline Point Cross(Point a_a, Point a_b)
		{
			return { a_a.y * a_b.z - a_a.z * a_b.y, a_a.z * a_b.x - a_a.x * a_b.z, a_a.x * a_b.y - a_a.y * a_b.x };
		}

		// Closest distance between TWO FINITE segments, including their endpoints.
		// No exclusion zone near the subject or lens, and no infinite ray extension.
		[[nodiscard]] inline float SegmentDistanceSquared(Point a_p, Point a_q, Point a_a, Point a_b)
		{
			if (!Finite(a_p) || !Finite(a_q) || !Finite(a_a) || !Finite(a_b)) {
				return std::numeric_limits<float>::quiet_NaN();
			}
			const auto d1 = Sub(a_q, a_p);
			const auto d2 = Sub(a_b, a_a);
			const auto r = Sub(a_p, a_a);
			const float aa = Dot(d1, d1);
			const float ee = Dot(d2, d2);
			const float ff = Dot(d2, r);
			constexpr float eps = 1.0e-6f;
			float s = 0.0f;
			float t = 0.0f;
			if (aa <= eps && ee <= eps) {
				return Dot(r, r);
			}
			if (aa <= eps) {
				t = std::clamp(ff / ee, 0.0f, 1.0f);
			} else {
				const float cc = Dot(d1, r);
				if (ee <= eps) {
					s = std::clamp(-cc / aa, 0.0f, 1.0f);
				} else {
					const float bb = Dot(d1, d2);
					const float denom = aa * ee - bb * bb;
					if (denom > eps) {
						s = std::clamp((bb * ff - cc * ee) / denom, 0.0f, 1.0f);
					}
					t = (bb * s + ff) / ee;
					if (t < 0.0f) {
						t = 0.0f;
						s = std::clamp(-cc / aa, 0.0f, 1.0f);
					} else if (t > 1.0f) {
						t = 1.0f;
						s = std::clamp((bb - cc) / aa, 0.0f, 1.0f);
					}
				}
			}
			const auto separation = Sub(Add(a_p, Mul(d1, s)), Add(a_a, Mul(d2, t)));
			return std::max(0.0f, Dot(separation, separation));
		}

		[[nodiscard]] inline bool SegmentHitsCapsule(Point a_p, Point a_q, Point a_a, Point a_b, float a_radius)
		{
			return std::isfinite(a_radius) && a_radius >= 0.0f &&
				SegmentDistanceSquared(a_p, a_q, a_a, a_b) <= a_radius * a_radius;
		}

		struct Frame
		{
			Point origin{}, forward{}, right{}, up{};
			float halfWidth{}, halfHeight{};  // tangent of the usable half angles
			bool valid{ false };
		};

		[[nodiscard]] inline Frame MakeFrame(Point a_camera, Point a_lookAt, float a_lens, float a_aspect, float a_crop)
		{
			Frame out{};
			if (!Finite(a_camera) || !Finite(a_lookAt) || !std::isfinite(a_lens) ||
				!std::isfinite(a_aspect) || !std::isfinite(a_crop) || a_lens <= 0.0f || a_lens >= 179.0f ||
				a_aspect <= 0.0f || a_crop < 0.0f || a_crop >= 0.5f) {
				return out;
			}
			const auto delta = Sub(a_lookAt, a_camera);
			const float length = std::sqrt(Dot(delta, delta));
			if (!std::isfinite(length) || length <= 1.0e-3f) {
				return out;
			}
			out.origin = a_camera;
			out.forward = Mul(delta, 1.0f / length);
			// The alternate reference makes directly overhead views well defined.
			const Point reference = std::abs(out.forward.z) > 0.999999f ? Point{ 0.0f, 1.0f, 0.0f } : Point{ 0.0f, 0.0f, 1.0f };
			out.right = Cross(out.forward, reference);
			out.right = Mul(out.right, 1.0f / std::sqrt(Dot(out.right, out.right)));
			out.up = Cross(out.right, out.forward);
			constexpr float radians = 0.017453292519943295f;
			out.halfWidth = std::tan(a_lens * radians * 0.5f);
			out.halfHeight = out.halfWidth / a_aspect * (1.0f - 2.0f * a_crop);
			out.valid = std::isfinite(out.halfWidth) && std::isfinite(out.halfHeight);
			return out;
		}

		[[nodiscard]] inline bool InFrame(const Frame& a_frame, Point a_sample)
		{
			if (!a_frame.valid || !Finite(a_sample)) {
				return false;
			}
			const auto offset = Sub(a_sample, a_frame.origin);
			const float depth = Dot(offset, a_frame.forward);
			return depth > 0.01f && std::abs(Dot(offset, a_frame.right)) <= depth * a_frame.halfWidth &&
				std::abs(Dot(offset, a_frame.up)) <= depth * a_frame.halfHeight;
		}

		[[nodiscard]] inline SightState FaceState(const std::array<SightState, 5>& a_samples)
		{
			std::size_t clear = 0;
			std::size_t unknown = 0;
			for (const auto state : a_samples) {
				clear += state == SightState::kClear;
				unknown += state == SightState::kUnknown;
			}
			if (a_samples[0] == SightState::kBlocked || clear + unknown < 4) {
				return SightState::kBlocked;
			}
			// Failed readings never become verified-clear, even when enough of the
			// remaining samples happen to pass the nominal threshold.
			if (unknown != 0) {
				return SightState::kUnknown;
			}
			return clear >= 4 ? SightState::kClear : SightState::kBlocked;
		}
	}
}
