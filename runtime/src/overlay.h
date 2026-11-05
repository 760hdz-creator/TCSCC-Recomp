// Extra Settings menu (F1 or L3+R3; Dear ImGui) drawn over the game: Display, Graphics and Update
// tabs, plus the update notification once the player signs in at the main menu. ImGui runs on the
// render thread; the window thread only toggles the menu and queues the input messages it takes.
#pragma once
#include <windows.h>
#include <settings.h>

class D3D11Renderer;
struct ImFont;
struct ID3D11Device;

// Drawn over the main menu page (inside the ImGui frame, render thread): the player card
// (player_card.cpp registers it; the test build has none).
using OverlayPlayerCard = void (*)(ImFont* body, ID3D11Device* device);
void OverlaySetPlayerCard(OverlayPlayerCard draw);

// Render thread, once the renderer is up: applies the settings (scale, post-processing) and draws
// the menu / notification over every presented frame.
void OverlayAttach(D3D11Renderer& renderer, HWND hwnd, const DisplaySettings& settings);

// Posted to the game window by the render thread when L3+R3 is pressed: toggle like F1.
constexpr UINT kOverlayToggleMessage = WM_APP + 1;
// Posted by OverlayRequestOpen: open the menu if it is closed.
constexpr UINT kOverlayOpenMessage = WM_APP + 2;
// Any thread (the game's OPTIONS > EXTRA SETTINGS button, menu_hooks.cpp): open the menu.
void OverlayRequestOpen();

// Any thread (the main menu's RECOMP SETTINGS page, main_menu.cpp): the display settings as the
// menu has them, and a change to them (applied and saved to settings.ini on the next frame).
DisplaySettings OverlaySettings();
void OverlayChangeSettings(const DisplaySettings& s);

// Window thread. F1 (and Esc while open) toggles the menu; returns the new state. The menu never
// opens by itself.
bool OverlayToggle();
bool OverlayOpen();
bool OverlayCapturingKey(); // the Controls tab waits for a key: Esc binds instead of closing
// While the menu is open, mouse and keyboard messages go to it (true: taken, not for the game).
bool OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
