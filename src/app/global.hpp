#pragma once

#include "defs.hpp"

struct vec2
{
	int x, y;
};

enum game_t : int
{
	UNIVERSAL = -1,
	NFSU,
	NFSU2,
	NFSC,
	NFSPS,
	NFSUC,
};

/// Stores cross-cutting runtime state shared by hooks, audio, and overlay code.
class global
{
public:
	static bool shutdown;

	/// Shows a message box attached to the current game window.
	static void msg_box(std::string title, std::string message)
	{
		MessageBoxA(global::hwnd, &message[0], &title[0], 0);
	}

	static std::vector<std::string> game_bins;

	static HMODULE self;
	static bool sys_init;
	static game_t game;
	static bool hide;
	static HWND hwnd;
	static kiero::RenderType::Enum renderer;
	static GameFlowState global::state;
};
