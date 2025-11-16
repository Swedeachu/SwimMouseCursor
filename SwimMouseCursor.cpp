// Standalone console utility to confine mouse to the Minecraft Bedrock window.
// Notes:
//  - Detects Bedrock by process name "Minecraft.Windows.exe". Falls back to window title contains "Minecraft".
//  - Clips cursor to window bounds whenever Minecraft is focused (fullscreen OR windowed)
//  - Configurable hotkey to recenter cursor (default: E key, configurable via config.txt)
//  - Uses low-level keyboard hook to NOT consume the key press

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <cctype>

#pragma comment(lib, "Shlwapi.lib")

#include "VirtualKeyParser.h"

static const wchar_t* TARGET_EXE = L"Minecraft.Windows.exe";
static const wchar_t* CONFIG_FILE = L"config.txt";
static std::atomic<bool> clippingEnabled{ true };
static std::atomic<bool> running{ true };
static std::atomic<bool> windowBeingMoved{ false };
static WORD recenterKey = 'E'; // Default recenter key
static HHOOK keyboardHook = nullptr;

static void Log(const wchar_t* fmt, ...)
{
	wchar_t buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
	va_end(ap);
	DWORD ignored;

	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut && hOut != INVALID_HANDLE_VALUE)
	{
		WriteConsoleW(hOut, buf, (DWORD)wcslen(buf), &ignored, nullptr);
		WriteConsoleW(hOut, L"\r\n", 2, &ignored, nullptr);
	}
	else
	{
		// Fallback
		wprintf(L"%s\n", buf);
	}
}

static std::wstring GetProcessExeName(DWORD pid)
{
	std::wstring name;
	if (!pid) return name;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return name;

	wchar_t buf[MAX_PATH];
	DWORD sz = MAX_PATH;
	if (QueryFullProcessImageNameW(h, 0, buf, &sz))
	{
		wchar_t* fname = PathFindFileNameW(buf);
		if (fname) name = fname;
	}
	CloseHandle(h);
	return name;
}

static bool IsMinecraftWindow(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd)) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	std::wstring exe = GetProcessExeName(pid);
	if (!exe.empty() && _wcsicmp(exe.c_str(), TARGET_EXE) == 0)
	{
		return true;
	}

	// Fallback: title contains "Minecraft"
	wchar_t title[512] = { 0 };
	GetWindowTextW(hwnd, title, 511);
	return wcsstr(title, L"Minecraft") != nullptr;
}

// Detect if any window is being moved or resized
static bool IsAnyWindowBeingMovedOrResized()
{
	// Check if left mouse button is down (used for dragging)
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
	{
		// Get cursor position
		POINT pt;
		if (GetCursorPos(&pt))
		{
			HWND hwndAtCursor = WindowFromPoint(pt);
			if (hwndAtCursor)
			{
				// Check if cursor is over a window's non-client area (title bar, borders)
				LRESULT hitTest = SendMessageW(hwndAtCursor, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));

				// If hit test returns any of these, a move/resize is likely happening
				if (hitTest == HTCAPTION ||      // Title bar (dragging to move)
					hitTest == HTLEFT ||         // Left border
					hitTest == HTRIGHT ||        // Right border
					hitTest == HTTOP ||          // Top border
					hitTest == HTTOPLEFT ||      // Top-left corner
					hitTest == HTTOPRIGHT ||     // Top-right corner
					hitTest == HTBOTTOM ||       // Bottom border
					hitTest == HTBOTTOMLEFT ||   // Bottom-left corner
					hitTest == HTBOTTOMRIGHT)    // Bottom-right corner
				{
					return true;
				}
			}
		}
	}

	return false;
}

static bool IsWindowActuallyVisibleAndTopmost(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
		return false;

	// Check if the window is minimized
	if (IsIconic(hwnd))
		return false;

	// CRITICAL: Window must be the actual foreground window receiving input
	HWND fgWindow = GetForegroundWindow();
	if (fgWindow != hwnd)
		return false;

	// Get the window rect
	RECT windowRect{};
	if (!GetWindowRect(hwnd, &windowRect))
		return false;

	// Check if window has any visible area
	if (windowRect.right <= windowRect.left || windowRect.bottom <= windowRect.top)
		return false;

	// Additional check: Get the GUI thread info to verify focus
	GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
	DWORD windowThreadId = GetWindowThreadProcessId(hwnd, nullptr);
	if (GetGUIThreadInfo(windowThreadId, &gti))
	{
		// If there's an active window in the thread, it should match our window
		if (gti.hwndActive && gti.hwndActive != hwnd)
		{
			// Check if active window belongs to same root
			HWND activeRoot = GetAncestor(gti.hwndActive, GA_ROOT);
			HWND ourRoot = GetAncestor(hwnd, GA_ROOT);
			if (activeRoot != ourRoot)
				return false;
		}
	}

	// More aggressive check - sample the CENTER of the window
	// If the center point doesn't belong to Minecraft, another window is definitely on top
	int centerX = (windowRect.left + windowRect.right) / 2;
	int centerY = (windowRect.top + windowRect.bottom) / 2;
	POINT centerPt = { centerX, centerY };

	HWND windowAtCenter = WindowFromPoint(centerPt);
	if (windowAtCenter)
	{
		HWND rootAtCenter = GetAncestor(windowAtCenter, GA_ROOT);
		HWND rootMinecraft = GetAncestor(hwnd, GA_ROOT);

		// If center doesn't belong to Minecraft, we're definitely covered
		if (rootAtCenter != rootMinecraft)
			return false;
	}

	// Sample multiple points across the window to ensure it's actually visible
	// This catches cases where another window is layered on top
	int numChecks = 0;
	int passedChecks = 0;

	// More aggressive sampling - check more points
	for (int x = windowRect.left + 10; x < windowRect.right - 10; x += (windowRect.right - windowRect.left) / 5)
	{
		for (int y = windowRect.top + 10; y < windowRect.bottom - 10; y += (windowRect.bottom - windowRect.top) / 5)
		{
			numChecks++;
			POINT pt = { x, y };
			HWND windowAtPoint = WindowFromPoint(pt);

			if (windowAtPoint)
			{
				HWND rootAtPoint = GetAncestor(windowAtPoint, GA_ROOT);
				HWND rootMinecraft = GetAncestor(hwnd, GA_ROOT);

				if (rootAtPoint == rootMinecraft)
					passedChecks++;
			}
		}
	}

	// STRICTER: Require 90% of sampled points to belong to Minecraft (was 75%)
	if (numChecks > 0 && passedChecks < (numChecks * 9 / 10))
		return false;

	// Final check: Verify no other window has captured input
	HWND captureWindow = GetCapture();
	if (captureWindow && captureWindow != hwnd)
	{
		HWND captureRoot = GetAncestor(captureWindow, GA_ROOT);
		HWND ourRoot = GetAncestor(hwnd, GA_ROOT);
		if (captureRoot != ourRoot)
			return false;
	}

	return true;
}

static bool GetWindowClipRect(HWND hwnd, RECT& outClipRect)
{
	if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;

	// Force a brief wait to ensure window has settled after focus change
	// This helps ensure GetWindowRect returns accurate dimensions. This is incredibly scuffed and C style hacky but works.
	// I am scared a high level sleep call might hurt/delay chained input across the windows messaging pipeline and program functionality on certain CPUs.
	// Sleep(5); // Just not going to do it then.

	// ALWAYS get fresh window rect - don't trust cached values
	RECT wr{};
	if (!GetWindowRect(hwnd, &wr)) return false;

	// Validate window rect is reasonable
	if (wr.right <= wr.left || wr.bottom <= wr.top)
		return false;

	// Get fresh client rect
	RECT clientRect{};
	if (!GetClientRect(hwnd, &clientRect))
	{
		// If client rect fails, use window rect as fallback
		outClipRect = wr;
		return true;
	}

	// Validate client rect
	if (clientRect.right <= 0 || clientRect.bottom <= 0)
	{
		// Invalid client rect, use window rect
		outClipRect = wr;
		return true;
	}

	// Create points for client area corners
	POINT topLeft = { 0, 0 };
	POINT bottomRight = { clientRect.right, clientRect.bottom };

	// CRITICAL: Always get fresh screen coordinates
	// Try multiple times to ensure we get valid coordinates
	bool convertSuccess = false;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (ClientToScreen(hwnd, &topLeft) && ClientToScreen(hwnd, &bottomRight))
		{
			// Validate the converted coordinates make sense
			if (bottomRight.x > topLeft.x && bottomRight.y > topLeft.y)
			{
				convertSuccess = true;
				break;
			}
		}

		// If conversion failed, wait briefly and try again, in testing this seems unreachable though (which is good).
		if (attempt < 2)
		{
			Sleep(5);
			// Reset points
			topLeft = { 0, 0 };
			bottomRight = { clientRect.right, clientRect.bottom };
		}
	}

	if (!convertSuccess)
	{
		// If all conversion attempts failed, use window rect
		outClipRect = wr;
		return true;
	}

	// Double-check that client area is actually within window rect
	// This catches weird edge cases
	if (topLeft.x < wr.left - 10 || topLeft.y < wr.top - 10 ||
		bottomRight.x > wr.right + 10 || bottomRight.y > wr.bottom + 10)
	{
		// Client area seems wrong, use window rect
		outClipRect = wr;
		return true;
	}

	// Use the client area (excludes title bar and borders)
	outClipRect.left = topLeft.x;
	outClipRect.top = topLeft.y;
	outClipRect.right = bottomRight.x;
	outClipRect.bottom = bottomRight.y;

	// Final sanity check on output rect
	if (outClipRect.right <= outClipRect.left || outClipRect.bottom <= outClipRect.top)
	{
		// Something went wrong, use window rect
		outClipRect = wr;
	}

	return true;
}

static void RecenterCursor(HWND hwnd)
{
	RECT wr{};
	if (GetWindowRect(hwnd, &wr))
	{
		int centerX = (wr.left + wr.right) / 2;
		int centerY = (wr.top + wr.bottom) / 2;
		SetCursorPos(centerX, centerY);
	}
}

static WORD LoadRecenterKeyFromConfig()
{
	// Try to open config file
	std::ifstream configFile(CONFIG_FILE);

	if (!configFile.is_open())
	{
		// File doesn't exist, create it with default value
		Log(L"[*] Config file not found. Creating %s with default key 'E'.", CONFIG_FILE);
		std::ofstream outFile(CONFIG_FILE);
		if (outFile.is_open())
		{
			outFile << "E";
			outFile.close();
		}
		return 'E';
	}

	// Read the file content
	std::string line;
	std::getline(configFile, line);
	configFile.close();

	// Trim whitespace
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
		line.pop_back();
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
		line.erase(0, 1);

	// Validate: should not be empty
	if (line.empty())
	{
		Log(L"[!] Config file is empty. Defaulting to 'E'.");
		return 'E';
	}

	// Use the VirtualKeyParser to parse the key name
	WORD parsedKey = VirtualKeyParser::ParseKeyName(line);

	if (parsedKey != 0)
	{
		std::string keyName = VirtualKeyParser::GetKeyNameFromVK(parsedKey);
		Log(L"[*] Loaded recenter key from config: '%S' (VK: 0x%02X)", keyName.c_str(), parsedKey);
		return parsedKey;
	}
	else
	{
		Log(L"[!] Invalid key name in config ('%S'). Defaulting to 'E'.", line.c_str());
		Log(L"[!] Valid examples: E, TAB, VK_TAB, SPACE, F1, CTRL, etc.");
		return 'E';
	}
}

// Low-level keyboard hook to detect recenter key without consuming it
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION)
	{
		KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

		// Only trigger on key down
		if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
		{
			HWND fg = GetForegroundWindow();

			// Check if Minecraft is focused AND actually visible
			if (fg && IsMinecraftWindow(fg) && IsWindowActuallyVisibleAndTopmost(fg))
			{
				// Check if it's the recenter key OR escape key
				if (kb->vkCode == recenterKey || kb->vkCode == VK_ESCAPE)
				{
					RecenterCursor(fg);
				}
			}
		}
	}

	// IMPORTANT: Return CallNextHookEx to NOT consume the key
	return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
		case CTRL_C_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
		running.store(false);
		ClipCursor(nullptr); // always release on exit
		return TRUE;
	}
	return FALSE;
}

int wmain(int argc, wchar_t** argv)
{
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

	Log(L"Bedrock Mouse Cursor, a Program to fix Minecraft Bedrock 1.21.121's Mouse Cursor Window Issues");
	Log(L"Programmed by Swedeachu, sponsored by discord.gg/swim");
	Log(L"Play Our MCPE Server: swimgg.club");
	Log(L"\n");

	// Load recenter key from config
	recenterKey = LoadRecenterKeyFromConfig();

	// Safety hotkey: Ctrl+Shift+C (this one can consume the key since it's a special combo)
	if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_SHIFT, 'C'))
	{
		Log(L"[!] Failed to register hotkey Ctrl+Shift+C (error %lu).", GetLastError());
	}
	else
	{
		Log(L"[*] Safety hotkey ready: Ctrl+Shift+C to toggle clipping on/off.");
	}

	// Install low-level keyboard hook for recenter key (non-blocking)
	keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
	if (!keyboardHook)
	{
		Log(L"[!] Failed to install keyboard hook (error %lu).", GetLastError());
	}
	else
	{
		std::string keyName = VirtualKeyParser::GetKeyNameFromVK(recenterKey);
		Log(L"[*] Recenter hotkey ready: Press '%S' to recenter cursor (non-blocking).", keyName.c_str());
	}

	Log(L"[*] CursorClipperConsole running. Looking for: %s", TARGET_EXE);
	Log(L"[*] Will clip cursor whenever Minecraft window is focused AND visible on screen.");
	Log(L"[*] Clipping is currently: ENABLED");

	// We'll pump messages only for hotkey; foreground tracking is via polling.
	MSG msg{};
	HWND lastActive = nullptr;
	bool lastClipped = false;
	bool needsClipUpdate = false;

	auto lastPoll = GetTickCount();
	const DWORD POLL_MS = 10;

	while (running.load())
	{
		// Non-blocking message pump (for hotkey and hook)
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_HOTKEY)
			{
				if (msg.wParam == 1)
				{
					// Toggle clipping on/off
					clippingEnabled.store(!clippingEnabled.load());
					if (!clippingEnabled.load())
					{
						ClipCursor(nullptr);
						lastClipped = false;
						Log(L"[=] Clipping DISABLED — cursor released.");
					}
					else
					{
						Log(L"[=] Clipping ENABLED — will clip when Minecraft is focused.");
					}
				}
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		DWORD now = GetTickCount();
		if (now - lastPoll >= POLL_MS)
		{
			lastPoll = now;

			// Check if any window is being moved or resized
			bool movingWindow = IsAnyWindowBeingMovedOrResized();
			if (movingWindow != windowBeingMoved.load())
			{
				windowBeingMoved.store(movingWindow);
				if (movingWindow)
				{
					Log(L"[~] Window move/resize detected — temporarily releasing cursor.");
					ClipCursor(nullptr);
					lastClipped = false;
				}
				else
				{
					Log(L"[~] Window move/resize ended — forcing clip rect update.");
					needsClipUpdate = true; // Force update clip rect after window change
				}
			}

			// If a window is being moved/resized, don't clip
			if (movingWindow)
			{
				if (lastClipped)
				{
					ClipCursor(nullptr);
					lastClipped = false;
				}
				std::this_thread::yield();
				continue;
			}

			HWND fg = GetForegroundWindow();

			// If clipping is disabled, always release
			if (!clippingEnabled.load())
			{
				if (lastClipped)
				{
					ClipCursor(nullptr);
					lastClipped = false;
				}
				std::this_thread::yield();
				continue;
			}

			if (fg != lastActive)
			{
				// Foreground changed - FORCE clip rect refresh
				if (fg && IsMinecraftWindow(fg))
				{
					Log(L"[+] Minecraft active - refreshing window geometry.");
					needsClipUpdate = true; // Force fresh clip rect calculation
				}
				else
				{
					if (lastClipped)
					{
						ClipCursor(nullptr);
						lastClipped = false;
						Log(L"[-] Minecraft not active — cursor released.");
					}
				}
				lastActive = fg;
			}

			// Check if Minecraft is foreground AND actually visible
			if (fg && IsMinecraftWindow(fg) && IsWindowActuallyVisibleAndTopmost(fg))
			{
				RECT clip{};
				// ALWAYS get fresh clip rect - never trust old values
				if (GetWindowClipRect(fg, clip))
				{
					// Validate clip rect is reasonable
					if (clip.right > clip.left && clip.bottom > clip.top)
					{
						// Check if clip rect actually changed significantly (moved/resized)
						RECT currentClip{};
						bool hasCurrentClip = (GetClipCursor(&currentClip) != 0);

						bool clipChanged = !hasCurrentClip ||
							abs(currentClip.left - clip.left) > 2 ||
							abs(currentClip.top - clip.top) > 2 ||
							abs(currentClip.right - clip.right) > 2 ||
							abs(currentClip.bottom - clip.bottom) > 2;

						// Log and update if this is first clip, forced update, or rect changed
						if (needsClipUpdate || !lastClipped || clipChanged)
						{
							Log(L"[#] Clipping cursor to Minecraft window (%ld,%ld)-(%ld,%ld).",
								clip.left, clip.top, clip.right, clip.bottom);
							ClipCursor(&clip);
							lastClipped = true;
							needsClipUpdate = false;
						}
						else
						{
							// Rect is the same, but still apply it to ensure consistency
							ClipCursor(&clip);
						}
					}
					else
					{
						// Invalid clip rect
						if (lastClipped)
						{
							ClipCursor(nullptr);
							lastClipped = false;
							Log(L"[-] Invalid clip rect — cursor released.");
						}
					}
				}
			}
			else
			{
				if (lastClipped)
				{
					ClipCursor(nullptr);
					lastClipped = false;
					Log(L"[-] Minecraft not visible — cursor released.");
				}
			}
		}

		// Be a good citizen
		std::this_thread::yield();
	}

	// Cleanup
	if (keyboardHook)
	{
		UnhookWindowsHookEx(keyboardHook);
	}

	ClipCursor(nullptr);
	UnregisterHotKey(nullptr, 1);
	Log(L"[*] Exiting. Cursor released.");

	return 0;
}
