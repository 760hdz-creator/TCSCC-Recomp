# Button prompts and keyboard/mouse (2026-11-05)

## How Conviction draws button icons
- UI text carries tokens `{Img.Buttons.IK_Joy1..16}` (and `{CtrlImg.HudBarButtons.<action>}`); the tag builder
  sub_82E79558 turns them into Scaleform `<img src='IK_JoyN' width=.. height=..>`. The HUD movie uses the same art.
- The art = 16 textures, 36x36 DXT5 (fmt 20): grey shapes (fill 969696, rim E2E2E2, white symbols).
  IK_Joy1..6 = A B X Y LB RB (rest: LT RT LS RS Back Start d-pad). No PC/keyboard art on the disc
  (`_root.FlashMenu.xbox` cleared changes nothing).
- The renderer knows them by BlockLinearHash (`src/prompts_table.inc`) and, for PlayStation or keyboard, converts the
  texture with our own art (prompts.cpp, GDI+ at 4x, Conviction's palette). Style + input-settings generation are
  folded into the texture hash, so a device switch or a rebinding converts them again.
- Collecting the table: `SCDA_PROMPT_DUMP=<dir>` (every unknown DXT5 square 24-64 px once per block hash, as
  `<hash>_<w>.bmp`, logged `[prompts] candidate`) + `SCDA_PROMPT_SHOWALL=<n>` (the difficulty page Rookie line and
  the Back tip show `{Img.Buttons.IK_Joy<n..16>}`). `SCDA_TRACE_FLASH=1` logs each `[img]` tag.
- `SCDA_PIXEL_WATCH` now also logs texture fetch slots 0-3 (base, format, size): how the 36x36 icon was found.
- Not replaced: the CONTROLLER LAYOUT page's controller picture (an Xbox pad drawing).

## Styles
- `[Input] ButtonPrompts=Auto|Xbox|PlayStation|Keyboard` (F1 > Controls). Auto = last device used (prompts.h tracker):
  DualShock 4 -> PS4 art (touchpad Back), DualSense -> PS5, keyboard/mouse -> the keys bound.
- PS: cross/circle/square/triangle discs, L1/R1/L2/R2 caps, L/R sticks, touchpad (Back), OPTIONS pill, 4-key d-pad.
- Keyboard: the first key bound to the control (key cap, arrow cap, or the mouse with the button lit); left stick
  = the 4 move keys, right stick = the mouse.

## Keyboard/mouse bindings (keybinds.h)
- `[Keyboard]` one line per control, keys comma-separated: `A=E, Enter`. Controls: A B X Y LB RB LT RT LS RS Back
  Start DpadUp DpadDown DpadLeft DpadRight MoveUp MoveDown MoveLeft MoveRight LookUp LookDown LookLeft LookRight
  Walk. Keys: letters, digits, F1-F12, Num0-9, Space Enter Esc Tab Backspace (L/R)Shift (L/R)Ctrl (L/R)Alt CapsLock
  arrows Insert Delete Home End PageUp PageDown LMB RMB MMB Mouse4 Mouse5 WheelUp WheelDown, punctuation names.
  `None` = unbound; a line that does not parse keeps the default.
- Defaults: WASD move, Shift walk (stick ~40 %), mouse look, LMB shoot, RMB aim (R3), Space cover, C/LCtrl crouch,
  E/Enter action-OK, F/Backspace attack-back, G gadget, Q execute, Z/MMB mark, T/Up sonar, X/Down/wheel weapon, R reload (L3),
  1/Left 2/Right gadgets, Tab help, Esc pause, IJKL look keys.
- F1 > Controls: rebinding (click, press; right-click clears; Esc can be bound while waiting), mouse sensitivity /
  invert (live), mouse look (next start), button prompts.
- Menus are pad-style (keys/arrows); the 360 menus have no mouse pointer.
