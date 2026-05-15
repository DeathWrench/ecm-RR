#pragma once

/// Updates the host window integration used by the overlay.
class window
{
public:
	/// Runs the per-frame window maintenance required by the overlay hooks.
	static void update();
};