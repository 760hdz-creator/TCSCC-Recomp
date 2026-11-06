# Main menu invisible after EXIT TO MAIN MENU (open, 2026-11-05)

Repro (copy of a LOCALAPPDATA with a 01_Market save; wall-clock script):
`SCDA_AUTOPRESS=240:START,400:A:20,500:A:20,600:A:20,3200:START:4,3300:UP:3,3400:A:4,3520:UP:3,3600:A:4`
(Continue Story, pause, UP = EXIT TO MAIN MENU, A, UP = Confirm, A). Screenshot ~160 s: only the CONVICTION title.

Facts:
- The menu works (input selects items); same draw count as a good menu (2075). Text draw (VS 9ee7d1e8 / PS fa8a940f,
  8x8 white texture, blend 07060706) runs but leaves the pixel unchanged: alpha 0.
- Same with FrameRate=30, DrawDistance=Console, SCDA_NO_SAVING_KEEP=1, SCDA_TEXHASH_COMPARE=1.
- The menu movie is made again on return (`_root.FlashMenu.init`, same flags). 3D menu object changes (418683C0 -> 41816580).
- Engine -> Flash diff (SCDA_TRACE_FLASH, `[flash>]` hooks on movie vtable 821DE880 slots 52-148): first boot also gets
  handleProfileChange (UI event 60), callbackObject.settings / soundValues / gamma / preset (vt+144/148/108), sound51On,
  cellModeUnlocked, onStorageDeviceChanged (event 81). They come from the profile object's message 4 handler
  sub_824F9420 (events 60, 74, 81, 79) after PRESS START.
- Replaying 60 (+74/81/79) after TO_MAIN (main_menu.cpp, SCDA_MENU_REFRESH=1 [+ _ALL]) delivers handleProfileChange,
  onStorageDeviceChanged, rollbackToIndex... but the items stay hidden. Event 60's handler travels TO_MAIN itself
  (re-entry guard needed: unguarded = stack overflow in sub_82E86A50).

Next: renderer A/B (SCDA_NO_CB_SKIP, SCDA_NO_FETCH_CACHE) in case the alpha is a stale constant; then find where the
text's alpha comes from (vertex colour vs PS constant cxform) with SCDA_TRACE_PSC on PS fa8a940f, good vs bad frame.
