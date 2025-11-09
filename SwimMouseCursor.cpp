// Standalone console utility to confine mouse to the Minecraft Bedrock fullscreen window.
// Notes:
//  - Detects Bedrock by process name "Minecraft.Windows.exe" (UWP). Falls back to window title contains "Minecraft".
//  - Clips only when the window exactly covers its monitor (true fullscreen). 

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

#pragma comment(lib, "Shlwapi.lib")

static const wchar_t* TARGET_EXE = L"Minecraft.Windows.exe";
static std::atomic<bool> g_clippingEnabled{ true };
static std::atomic<bool> g_running{ true };

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

static bool IsFullscreenOnAMonitor(HWND hwnd, RECT& outClipRect)
{
	if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;

	RECT wr{};
	if (!GetWindowRect(hwnd, &wr)) return false;

	HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (!mon) return false;

	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, &mi)) return false;

	RECT mr = mi.rcMonitor;

	// DEBUG: Log the actual values to diagnose issues
	static bool loggedOnce = false;
	if (!loggedOnce)
	{
		Log(L"[DBG] Window: (%ld,%ld)-(%ld,%ld) [%ldx%ld]",
			wr.left, wr.top, wr.right, wr.bottom,
			wr.right - wr.left, wr.bottom - wr.top);
		Log(L"[DBG] Monitor: (%ld,%ld)-(%ld,%ld) [%ldx%ld]",
			mr.left, mr.top, mr.right, mr.bottom,
			mr.right - mr.left, mr.bottom - mr.top);
		loggedOnce = true;
	}

	// More lenient tolerance for borderless fullscreen (handles DPI scaling, borders, etc.)
	auto nearEq = [](int a, int b) { return abs(a - b) <= 8; };
	bool covers =
		nearEq(wr.left, mr.left) &&
		nearEq(wr.top, mr.top) &&
		nearEq(wr.right, mr.right) &&
		nearEq(wr.bottom, mr.bottom);

	if (covers)
	{
		outClipRect = mr; // use monitor bounds (not work area)
		return true;
	}

	// Alternative: Check if window covers most of the monitor (90%+ area)
	// This handles borderless fullscreen even better
	int wWidth = wr.right - wr.left;
	int wHeight = wr.bottom - wr.top;
	int mWidth = mr.right - mr.left;
	int mHeight = mr.bottom - mr.top;

	float areaCoverage = (float)(wWidth * wHeight) / (float)(mWidth * mHeight);
	if (areaCoverage >= 0.90f) // 90% or more coverage = treat as fullscreen
	{
		outClipRect = mr;
		return true;
	}

	return false;
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
		g_running.store(false);
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

	// Safety hotkey: Ctrl+Shift+C
	if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_SHIFT, 'C'))
	{
		Log(L"[!] Failed to register hotkey Ctrl+Shift+C (error %lu).", GetLastError());
	}
	else
	{
		Log(L"[*] Safety hotkey ready: Ctrl+Shift+C to toggle clipping on/off.");
	}

	Log(L"[*] CursorClipperConsole running. Looking for: %s", TARGET_EXE);
	Log(L"[*] Will clip ONLY when the game is truly fullscreen on a monitor.");
	Log(L"[*] Clipping is currently: ENABLED");

	// We'll pump messages only for hotkey; foreground tracking is via polling.
	MSG msg{};
	HWND lastActive = nullptr;
	bool lastClipped = false;

	auto lastPoll = GetTickCount();
	const DWORD POLL_MS = 10;

	while (g_running.load())
	{
		// Non-blocking message pump (for hotkey)
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_HOTKEY && msg.wParam == 1)
			{
				// Toggle clipping on/off
				g_clippingEnabled.store(!g_clippingEnabled.load());
				if (!g_clippingEnabled.load())
				{
					ClipCursor(nullptr);
					lastClipped = false;
					Log(L"[=] Clipping DISABLED — cursor released.");
				}
				else
				{
					Log(L"[=] Clipping ENABLED — will clip when fullscreen.");
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
			if (!g_clippingEnabled.load())
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

			if (fg && IsMinecraftWindow(fg))
			{
				RECT clip{};
				if (IsFullscreenOnAMonitor(fg, clip))
				{
					if (!lastClipped)
					{
						Log(L"[#] Entered fullscreen — clipping to monitor (%ld,%ld)-(%ld,%ld).",
							clip.left, clip.top, clip.right, clip.bottom);
					}
					ClipCursor(&clip);
					lastClipped = true;
				}
				else
				{
					if (lastClipped)
					{
						Log(L"[#] Not fullscreen anymore — releasing cursor.");
						ClipCursor(nullptr);
						lastClipped = false;
					}
				}
			}
			else
			{
				if (lastClipped)
				{
					ClipCursor(nullptr);
					lastClipped = false;
				}
			}
		}

		// Be a good citizen
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	// Cleanup
	ClipCursor(nullptr);
	UnregisterHotKey(nullptr, 1);
	Log(L"[*] Exiting. Cursor released.");

	return 0;
}
