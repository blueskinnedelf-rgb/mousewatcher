# Mouse1 Watcher

**Download:** go to the [Releases](../../releases/latest) page and grab `Mouse1Watcher.zip`. The exe is built by GitHub from the source in this repo (see the Actions tab), so you can check exactly what it is.

A small (60 KB) Windows tool that watches the left mouse button and writes down exactly where clicks go missing. You don't need to install anything.

## Running it

**Use `Mouse1Watcher.exe`.** The window title, the top of the window and the first line of the log all say **VERSION 4**. Right-click the exe, then Properties, then Details, and it shows File version 4.0. If it doesn't say 4, it's an old copy.

1. Put `Mouse1Watcher.exe` in its own folder, like `Documents\Mouse1Watcher`. The logs get saved in that folder.
2. Double-click it. Windows will probably say **"Windows protected your PC"** because the program isn't signed. Click **More info**, then **Run anyway**.
3. **Quick check:** click a few times anywhere. In the watcher window, `clicks mouse=` and `windows=` should both go up. If they do, it's working.
4. Leave the window open (you can minimize it) and play as usual.
5. **When left click stops working, press the marker key shown at the top of the watcher window.** It is usually `Ctrl + Alt + M`, but if another app already uses that, it switches to `Scroll Lock` or `Pause`. You'll hear a beep, and the watcher saves a marker and a full snapshot of what's going on.
6. Once you've had it happen once or twice, close the window and send the `mouse1_log_....txt` file.

## Is it safe with anti-cheat (CS2 / Apex)?

It's built to stay out of anti-cheat's way. It only uses the same standard Windows mouse input that Discord, OBS and games use, and it checks the button state every few milliseconds.

It **doesn't**:
- hook the mouse
- inject anything into a game
- open, read or touch the game process
- send any input

Nothing about it looks like a cheat. I can't promise how any anti-cheat will behave, though. If she wants to be extra careful, she can run it during casual or unranked matches first.

## What the log can tell us

| Tag | Meaning | What it points to |
|---|---|---|
| `SWALLOWED` | The mouse sent a click, but Windows never registered it | Some software is eating clicks: an overlay, a macro or RGB tool (Synapse, G Hub, iCUE), AutoHotkey, a mouse hook |
| `STUCK` | Windows thinks left is held down, but no mouse is pressing it | A lost "release" event. This often feels like click "stops working until you click again" |
| `PHANTOM` | Windows got a press that no mouse sent | Software is sending fake clicks |
| `INJECTED` | A click arrived from software, not from a device | Same as above, and it names it directly |
| `DESKTOP` | Windows switched to the secure screen (UAC prompt, lock, Ctrl+Alt+Del) | A hidden UAC prompt grabbing input |
| `SWAP` | The left/right button swap setting changed | Something is toggling the mouse settings |
| `SILENCE` | A mouse sent nothing at all for a few seconds while left was held | The wireless link or USB dropped out |
| `DEVICE` | A mouse connected or disconnected | USB or driver resets |
| `FOCUS` | Which window was active | Lines the problem up with a specific app |
| `MINUTE` | Mouse clicks vs. Windows clicks for each minute; `MISMATCH` means they differ | A quick health check |

If the click dies and the log shows **nothing** at the marker (no mouse presses at all), the clicks aren't even reaching Windows from either mouse. That points at USB, the chipset or a driver, not at an app.

## Files

- `Mouse1Watcher.exe`: the program
- `mouse1_watcher.c`: the full source code, in case anyone wants to check what it does

