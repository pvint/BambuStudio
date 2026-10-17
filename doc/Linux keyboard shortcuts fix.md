# Linux: keyboard shortcuts ignored until you click the 3D view

Branch `fix/linux-hotkey-focus`, based on `v02.09.01.70`.

## Symptom

On Linux, keyboard shortcuts such as **Ctrl+I** (import), **Delete**, **Ctrl+S** or the 3D view keys
often do nothing. Clicking the build plate once or twice makes them work again. It looks random:
the same steps sometimes work and sometimes don't.

Related reports:

- bambulab/BambuStudio [#10241](https://github.com/bambulab/BambuStudio/issues/10241) (hotkeys don't work in the Objects tab),
  [#4527](https://github.com/bambulab/BambuStudio/issues/4527) (Ctrl+Q), [#2756](https://github.com/bambulab/BambuStudio/issues/2756) (Delete works every other time)
- OrcaSlicer [#10268](https://github.com/OrcaSlicer/OrcaSlicer/issues/10268) (must click twice),
  [#9682](https://github.com/OrcaSlicer/OrcaSlicer/issues/9682) (hotkeys not working on Ubuntu)

## Cause

On GTK, a key press goes to the window, which hands it only to the widget that has keyboard focus
inside that window. wxWidgets builds its events from there: `wxEVT_CHAR_HOOK` travels up from the
focused widget to `MainFrame`, which handles the application shortcuts (Ctrl+I, Ctrl+S, Ctrl+O,
...), and the 3D canvas handles its own keys (Delete, A, M, ...) when it has the focus itself.
**If no widget in the window has the focus, GTK drops the key** and wxWidgets never sees it.

GTK clears a window's focus when the focused widget is hidden or disabled. Bambu Studio does both
routinely and never gives the focus back:

1. **Loading models.** `Plater::priv::load_files()` shows an app-modal `ProgressDialog`
   ("Loading...", `wxPD_APP_MODAL`), which disables the main window while it runs
   (`ProgressDialog::DisableOtherWindows()`). GTK takes the focus away from the disabled canvas,
   and re-enabling the window doesn't restore it. So the first shortcut after every import was
   lost. This is the most common trigger.
2. **Switching main tabs** (e.g. Home or Device back to Prepare). The page that held the focus is
   hidden, and on GTK the new page is deliberately not focused: commit `cd8a9786a` removed that
   to stop Bambu Studio stealing focus from other applications whenever a printer event switched
   tabs ([#3046](https://github.com/bambulab/BambuStudio/issues/3046)).

Clicking the plate helped because `GLCanvas3D::on_mouse()` focuses the canvas on a click. Sometimes
it took a second click: that call was skipped when `wxWindow::FindFocus()` already returned the
canvas, and wxGTK's `FindFocus()` returns the last *requested* focus while one is pending
(`gs_pendingFocus`), so it could name the canvas while GTK's real focus was elsewhere. The fix
below doesn't change clicking; it makes the click unnecessary.

## Fix

**`fix(linux): focus the current page when a key press finds no focus`** (`MainFrame.cpp`)

`MainFrame` connects to the GTK `"event"` signal of its own window, which runs before GTK routes a
key press to the focused widget. If the window has no focused widget, or it is hidden, the handler
gives the focus to the current page (the 3D canvas on Prepare/Preview) and lets the key press
continue. GTK then delivers that same key to the canvas, so the key that found no focus is not
lost.

- It only acts on key presses GTK delivers to the main window, which happens only while that window
  is the active one. It can't steal focus from other applications, so #3046 stays fixed.
- It never moves the focus away from a widget that has it, such as a text field or the object list.
- It covers every way of losing the focus, not just the two above.

## Testing

This reproduces the bug every time without the fix:

1. Open a project and click the build plate once.
2. Press **Ctrl+I**, pick a model and let it load. Cancelling the file dialog isn't enough; the
   "Loading..." dialog has to appear.
3. Without touching the mouse, press **Ctrl+I** again, or **Delete** (the new object is selected).

Without the fix nothing happens; with it, the file dialog opens again or the object is deleted.

## Not covered

Clicking a main tab gives that tab button the keyboard focus (Bambu Studio's `Button` accepts focus
by default). The application shortcuts still work then, but 3D view keys such as Delete would go to
the button. This hasn't been reproduced in practice. If it shows up, making the tab buttons
non-focusable (`btn->SetCanFocus(false)` in `ButtonsListCtrl::InsertPage()`, `Notebook.cpp`) would
hand those keys to the fix above.

## Building this branch

The branch holds only the fix. On current Linux distros it may also need the build changes from
branch `build/gentoo-cmake`, e.g. `build(linux): link OSMesa only if present`: Mesa 25.1 removed
libOSMesa, which broke the link.

Source builds default to `BBL_INTERNAL_TESTING=1` (`version.inc`). They keep their settings in
`~/.config/BambuStudioInternal` instead of `~/.config/BambuStudio`, and the splash screen says
"Internal Version". To run one with the regular settings, start it with
`--datadir ~/.config/BambuStudio`, or configure with `-DBBL_INTERNAL_TESTING=0`.
