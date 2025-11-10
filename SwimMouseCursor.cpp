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

	// Sample multiple points across the window to ensure it's actually visible
	// This catches cases where another window is layered on top
	int numChecks = 0;
	int passedChecks = 0;

	for (int x = windowRect.left + 10; x < windowRect.right - 10; x += (windowRect.right - windowRect.left) / 4)
	{
		for (int y = windowRect.top + 10; y < windowRect.bottom - 10; y += (windowRect.bottom - windowRect.top) / 4)
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

	// At least 75% of sampled points must belong to Minecraft
	if (numChecks > 0 && passedChecks < (numChecks * 3 / 4))
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

	RECT wr{};
	if (!GetWindowRect(hwnd, &wr)) return false;

	// Clip to the window's client area for better experience
	RECT clientRect{};
	if (GetClientRect(hwnd, &clientRect))
	{
		POINT topLeft = { clientRect.left, clientRect.top };
		POINT bottomRight = { clientRect.right, clientRect.bottom };

		// Convert client coordinates to screen coordinates
		if (ClientToScreen(hwnd, &topLeft) && ClientToScreen(hwnd, &bottomRight))
		{
			outClipRect.left = topLeft.x;
			outClipRect.top = topLeft.y;
			outClipRect.right = bottomRight.x;
			outClipRect.bottom = bottomRight.y;
			return true;
		}
	}

	// Fallback to window rect if client rect fails
	outClipRect = wr;
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

	Log(L"Swim Mouse Cursor, a Program to fix Minecraft Bedrock 1.21.121's Mouse Cursor Window Issues");
	Log(L"By Swedeachu/Swimfan72: discord.gg/swim");
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

			HWND fg = GetForegroundWindow();

			// If clipping is disabled, always release
			if (!clippingEnabled.load())
			{
				if (lastClipped)
				{
					ClipCursor(nullptr);
					lastClipped = false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}

			if (fg != lastActive)
			{
				// Foreground changed
				if (fg && IsMinecraftWindow(fg))
				{
					Log(L"[+] Minecraft active.");
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
				if (GetWindowClipRect(fg, clip))
				{
					if (!lastClipped)
					{
						Log(L"[#] Clipping cursor to Minecraft window (%ld,%ld)-(%ld,%ld).",
							clip.left, clip.top, clip.right, clip.bottom);
					}
					ClipCursor(&clip);
					lastClipped = true;
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
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
