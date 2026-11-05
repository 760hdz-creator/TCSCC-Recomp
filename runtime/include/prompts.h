// In-game button prompts for the device in use. Conviction draws its prompts from 16 icon textures
// (36x36 DXT5; UI tokens {Img.Buttons.IK_Joy1..16} and the HUD); the renderer knows them by
// BlockLinearHash (table: src/prompts_table.inc, docs/prompts.md) and swaps in PlayStation or
// keyboard/mouse icons drawn here. Xbox keeps the game's own.
#pragma once
#include <cstdint>
#include <vector>
#include <pad.h>

enum class PromptStyle { Xbox, PS4, PS5, Keyboard };

// [Input] ButtonPrompts=Auto|Xbox|PlayStation|Keyboard (Auto: the device last used).
enum class PromptSetting { Auto, Xbox, PlayStation, Keyboard };
PromptSetting ParsePromptSetting(const char* value); // unknown -> Auto

// Auto: a keyboard/mouse input picks Keyboard, a pad input the pad's style; before any input, a
// connected pad, else Keyboard. A fixed setting wins; PlayStation means PS4 only for a DualShock 4
// (last PS pad seen), else PS5.
struct PromptTracker
{
    int last = -1;               // PromptStyle of the last input, -1 none yet
    PadKind lastPs = PadKind::None;
    void Note(PadKind pad, bool padActive, bool keyboardActive);
    PromptStyle Style(PromptSetting setting, PadKind connected) const;
};

// The runtime's tracker (XamInputGetState feeds it; the renderer reads it). Thread-safe.
void PromptNoteInput(PadKind pad, bool padActive, bool keyboardActive);
PromptStyle CurrentPromptStyle();

// Icon index for a texture's BlockLinearHash and size, -1 when it is not a prompt icon.
int PromptIconIndex(uint64_t blockHash, uint32_t width, uint32_t height);
const char* PromptIconName(int index);
// Redraws icon `index` for `style` over width*height RGBA8 texels (R in the low byte, straight
// alpha). False (texels untouched) for Xbox.
bool PromptRedraw(int index, PromptStyle style, std::vector<uint32_t>& texels, uint32_t width, uint32_t height);
// Loading-screen strip Screens\<lang>\Continue.bmp has its A button baked in: redraw it for
// `style` in place (whole BMP file bytes). False (untouched) for Xbox or an unknown layout.
bool PromptPatchContinueBmp(std::vector<uint8_t>& file, PromptStyle style);
