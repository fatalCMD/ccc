#pragma once

#include <cmath>
#include <optional>
#include <utility>

namespace SD::Scene
{
	// Node owns the display object's lifetime and supplies property reads/writes.
	// Restores always target that object, never a path in a replacement movie.
	template <class Node>
	class DialogueDisplayOverride
	{
	public:
		void Bind(Node a_node)
		{
			if (node == a_node) {
				return;
			}
			Reset();
			node = std::move(a_node);
		}

		bool SetAlpha(double a_alpha)
		{
			// Full opacity means relinquish our fade. It must not reveal a clip
			// that the movie itself keeps hidden while constructing its rows.
			if (a_alpha >= 100.0) {
				return RestoreAlpha();
			}
			double actual{};
			if (!node.ReadAlpha(actual)) {
				return false;
			}
			if (std::abs(actual - a_alpha) < 0.5) {
				return true;
			}
			if (!node.WriteAlpha(a_alpha)) {
				return false;
			}
			if (!originalAlpha) {
				originalAlpha = actual;
			}
			writtenAlpha = a_alpha;
			return true;
		}

		bool SetHidden(bool a_hidden)
		{
			if (!a_hidden) {
				return RestoreVisible();
			}
			bool actual{};
			if (!node.ReadVisible(actual)) {
				return false;
			}
			if (!actual) {
				return true;  // already hidden; no visibility debt belongs to us
			}
			if (!node.WriteVisible(false)) {
				return false;
			}
			originalVisible = true;
			return true;
		}

		bool Release()
		{
			const bool alphaDone = RestoreAlpha();
			const bool visibleDone = RestoreVisible();
			return alphaDone && visibleDone;
		}

		void Reset()
		{
			Release();
			// A failed restore may retry only while this object is still bound.
			// Dropping it must never transfer its debt to another object.
			node = {};
			originalAlpha.reset();
			originalVisible.reset();
		}

	private:
		bool RestoreAlpha()
		{
			if (!originalAlpha) {
				return true;
			}
			double actual{};
			if (!node.ReadAlpha(actual)) {
				return false;
			}
			if (actual == writtenAlpha && !node.WriteAlpha(*originalAlpha)) {
				return false;
			}
			originalAlpha.reset();  // also relinquish if another writer changed it
			return true;
		}

		bool RestoreVisible()
		{
			if (!originalVisible) {
				return true;
			}
			bool actual{};
			if (!node.ReadVisible(actual)) {
				return false;
			}
			if (!actual && !node.WriteVisible(*originalVisible)) {
				return false;
			}
			originalVisible.reset();
			return true;
		}

		Node node{};
		std::optional<double> originalAlpha;
		std::optional<bool> originalVisible;
		double writtenAlpha{};
	};
}
