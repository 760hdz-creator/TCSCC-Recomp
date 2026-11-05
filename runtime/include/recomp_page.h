// The main menu's RECOMP SETTINGS page: main_menu.cpp plays the menu's camera travel (as SETTINGS does)
// and opens it; it is drawn over the menu scene in the game's own style (overlay.cpp, render thread).
// While it is open, XamInputGetState gives its pad/keyboard state to the page and an idle pad to the game.
#pragma once
#include <windows.h>
#include <xinput.h>

struct ImFont;

void RecompPageOpen();
void RecompPageClose();  // without the travel back (the menu left the front end)
bool RecompPageIsOpen();
// B was pressed (and passed on to the game's page): true once, within 2 s; the caller sends the
// page's travel to the main menu.
bool RecompPageTakeBack();
void RecompPageInput(const XINPUT_GAMEPAD& g); // game thread, every input poll
// Render thread, inside an ImGui frame: fonts baked at title / body pixel sizes for this window.
void RecompPageDraw(ImFont* title, ImFont* body);
// The main menu's own page (IndexPage) is the current one: main_menu.cpp sets it from every
// TRAVEL_MENU event; the player card (player_card.cpp) shows only there.
void MainMenuSetIndexPage(bool on);
bool MainMenuOnIndexPage();
