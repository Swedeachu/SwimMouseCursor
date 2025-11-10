// VirtualKeyParser.h
// Header file for translating string key names to Windows Virtual Key codes
// Supports common key names, virtual key names (VK_*), and single characters

#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace VirtualKeyParser
{

	// Convert string to uppercase for case-insensitive comparison
	inline std::string ToUpper(const std::string& str)
	{
		std::string result = str;
		std::transform(result.begin(), result.end(), result.begin(),
			[](unsigned char c) { return std::toupper(c); });
		return result;
	}

	// Map of common key name strings to virtual key codes
	inline const std::unordered_map<std::string, WORD>& GetKeyNameMap()
	{
		static const std::unordered_map<std::string, WORD> keyMap = {
			// Letter keys (A-Z)
			{"A", 'A'}, {"B", 'B'}, {"C", 'C'}, {"D", 'D'}, {"E", 'E'},
			{"F", 'F'}, {"G", 'G'}, {"H", 'H'}, {"I", 'I'}, {"J", 'J'},
			{"K", 'K'}, {"L", 'L'}, {"M", 'M'}, {"N", 'N'}, {"O", 'O'},
			{"P", 'P'}, {"Q", 'Q'}, {"R", 'R'}, {"S", 'S'}, {"T", 'T'},
			{"U", 'U'}, {"V", 'V'}, {"W", 'W'}, {"X", 'X'}, {"Y", 'Y'},
			{"Z", 'Z'},

			// Number keys (0-9)
			{"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
			{"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},

			// Function keys
			{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
			{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
			{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
			{"VK_F1", VK_F1}, {"VK_F2", VK_F2}, {"VK_F3", VK_F3}, {"VK_F4", VK_F4},
			{"VK_F5", VK_F5}, {"VK_F6", VK_F6}, {"VK_F7", VK_F7}, {"VK_F8", VK_F8},
			{"VK_F9", VK_F9}, {"VK_F10", VK_F10}, {"VK_F11", VK_F11}, {"VK_F12", VK_F12},

			// Special keys
			{"SPACE", VK_SPACE}, {"VK_SPACE", VK_SPACE}, {"SPACEBAR", VK_SPACE},
			{"ENTER", VK_RETURN}, {"VK_RETURN", VK_RETURN}, {"RETURN", VK_RETURN},
			{"VK_ENTER", VK_RETURN},
			{"TAB", VK_TAB}, {"VK_TAB", VK_TAB},
			{"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"VK_ESCAPE", VK_ESCAPE},
			{"BACKSPACE", VK_BACK}, {"VK_BACK", VK_BACK}, {"BACK", VK_BACK},
			{"DELETE", VK_DELETE}, {"VK_DELETE", VK_DELETE}, {"DEL", VK_DELETE},
			{"INSERT", VK_INSERT}, {"VK_INSERT", VK_INSERT}, {"INS", VK_INSERT},
			{"HOME", VK_HOME}, {"VK_HOME", VK_HOME},
			{"END", VK_END}, {"VK_END", VK_END},
			{"PAGEUP", VK_PRIOR}, {"VK_PRIOR", VK_PRIOR}, {"PGUP", VK_PRIOR},
			{"PAGEDOWN", VK_NEXT}, {"VK_NEXT", VK_NEXT}, {"PGDN", VK_NEXT},

			// Arrow keys
			{"LEFT", VK_LEFT}, {"VK_LEFT", VK_LEFT},
			{"RIGHT", VK_RIGHT}, {"VK_RIGHT", VK_RIGHT},
			{"UP", VK_UP}, {"VK_UP", VK_UP},
			{"DOWN", VK_DOWN}, {"VK_DOWN", VK_DOWN},

			// Modifier keys
			{"SHIFT", VK_SHIFT}, {"VK_SHIFT", VK_SHIFT},
			{"LSHIFT", VK_LSHIFT}, {"VK_LSHIFT", VK_LSHIFT},
			{"RSHIFT", VK_RSHIFT}, {"VK_RSHIFT", VK_RSHIFT},
			{"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"VK_CONTROL", VK_CONTROL},
			{"LCTRL", VK_LCONTROL}, {"LCONTROL", VK_LCONTROL}, {"VK_LCONTROL", VK_LCONTROL},
			{"RCTRL", VK_RCONTROL}, {"RCONTROL", VK_RCONTROL}, {"VK_RCONTROL", VK_RCONTROL},
			{"ALT", VK_MENU}, {"VK_MENU", VK_MENU},
			{"LALT", VK_LMENU}, {"VK_LMENU", VK_LMENU},
			{"RALT", VK_RMENU}, {"VK_RMENU", VK_RMENU},

			// Numpad keys
			{"NUMPAD0", VK_NUMPAD0}, {"VK_NUMPAD0", VK_NUMPAD0},
			{"NUMPAD1", VK_NUMPAD1}, {"VK_NUMPAD1", VK_NUMPAD1},
			{"NUMPAD2", VK_NUMPAD2}, {"VK_NUMPAD2", VK_NUMPAD2},
			{"NUMPAD3", VK_NUMPAD3}, {"VK_NUMPAD3", VK_NUMPAD3},
			{"NUMPAD4", VK_NUMPAD4}, {"VK_NUMPAD4", VK_NUMPAD4},
			{"NUMPAD5", VK_NUMPAD5}, {"VK_NUMPAD5", VK_NUMPAD5},
			{"NUMPAD6", VK_NUMPAD6}, {"VK_NUMPAD6", VK_NUMPAD6},
			{"NUMPAD7", VK_NUMPAD7}, {"VK_NUMPAD7", VK_NUMPAD7},
			{"NUMPAD8", VK_NUMPAD8}, {"VK_NUMPAD8", VK_NUMPAD8},
			{"NUMPAD9", VK_NUMPAD9}, {"VK_NUMPAD9", VK_NUMPAD9},

			// Punctuation and symbols
			{"SEMICOLON", VK_OEM_1}, {"VK_OEM_1", VK_OEM_1},
			{"PLUS", VK_OEM_PLUS}, {"VK_OEM_PLUS", VK_OEM_PLUS},
			{"COMMA", VK_OEM_COMMA}, {"VK_OEM_COMMA", VK_OEM_COMMA},
			{"MINUS", VK_OEM_MINUS}, {"VK_OEM_MINUS", VK_OEM_MINUS},
			{"PERIOD", VK_OEM_PERIOD}, {"VK_OEM_PERIOD", VK_OEM_PERIOD},
			{"SLASH", VK_OEM_2}, {"VK_OEM_2", VK_OEM_2},
			{"TILDE", VK_OEM_3}, {"VK_OEM_3", VK_OEM_3},
			{"LEFTBRACKET", VK_OEM_4}, {"VK_OEM_4", VK_OEM_4},
			{"BACKSLASH", VK_OEM_5}, {"VK_OEM_5", VK_OEM_5},
			{"RIGHTBRACKET", VK_OEM_6}, {"VK_OEM_6", VK_OEM_6},
			{"QUOTE", VK_OEM_7}, {"VK_OEM_7", VK_OEM_7},
		};
		return keyMap;
	}

	// Parse a key name string and return the virtual key code
	// Returns 0 if the key name is invalid
	inline WORD ParseKeyName(const std::string& keyName)
	{
		if (keyName.empty())
			return 0;

		// Convert to uppercase for case-insensitive matching
		std::string upperKey = ToUpper(keyName);

		// Trim whitespace
		while (!upperKey.empty() && std::isspace(static_cast<unsigned char>(upperKey.back())))
			upperKey.pop_back();
		while (!upperKey.empty() && std::isspace(static_cast<unsigned char>(upperKey.front())))
			upperKey.erase(0, 1);

		if (upperKey.empty())
			return 0;

		// Look up in the key map
		const auto& keyMap = GetKeyNameMap();
		auto it = keyMap.find(upperKey);
		if (it != keyMap.end())
		{
			return it->second;
		}

		// If it's a single character A-Z or 0-9, return its virtual key code
		if (upperKey.length() == 1)
		{
			char c = upperKey[0];
			if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			{
				return static_cast<WORD>(c);
			}
		}

		// Not found
		return 0;
	}

	// Get a human-readable name for a virtual key code
	inline std::string GetKeyNameFromVK(WORD vkCode)
	{
		// Check single letters and numbers first
		if ((vkCode >= 'A' && vkCode <= 'Z') || (vkCode >= '0' && vkCode <= '9'))
		{
			return std::string(1, static_cast<char>(vkCode));
		}

		// Special cases for common keys
		switch (vkCode)
		{
			case VK_SPACE: return "SPACE";
			case VK_RETURN: return "ENTER";
			case VK_TAB: return "TAB";
			case VK_ESCAPE: return "ESC";
			case VK_BACK: return "BACKSPACE";
			case VK_DELETE: return "DELETE";
			case VK_INSERT: return "INSERT";
			case VK_HOME: return "HOME";
			case VK_END: return "END";
			case VK_PRIOR: return "PAGEUP";
			case VK_NEXT: return "PAGEDOWN";
			case VK_LEFT: return "LEFT";
			case VK_RIGHT: return "RIGHT";
			case VK_UP: return "UP";
			case VK_DOWN: return "DOWN";
			case VK_SHIFT: return "SHIFT";
			case VK_CONTROL: return "CTRL";
			case VK_MENU: return "ALT";
			case VK_F1: return "F1";
			case VK_F2: return "F2";
			case VK_F3: return "F3";
			case VK_F4: return "F4";
			case VK_F5: return "F5";
			case VK_F6: return "F6";
			case VK_F7: return "F7";
			case VK_F8: return "F8";
			case VK_F9: return "F9";
			case VK_F10: return "F10";
			case VK_F11: return "F11";
			case VK_F12: return "F12";
			default: return "UNKNOWN";
		}
	}

}