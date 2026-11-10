# Background shortcuts

Run a Home Assistant action with a controller combo **while a game is running** — dim the lights for a horror game, mute the soundbar, start the coffee machine — without leaving the game or picking up your phone.

It has two parts:

- The **Shortcuts screen** in Home5, where you set up what each combo does.
- A small **reader payload** (`PPSA11019-reader.elf`) that stays running and reads the controller in the background.

## Setting up a shortcut

Open **Options › Shortcuts**. For each shortcut you set:

- **Name** — what it's called.
- **What it does** — the Home Assistant action: a scene, script, automation, or a service call on a light, switch, fan, cover and so on. There's a filter row (Lights, Switches, Scenes, **Automations**, …) and a search to find it.
- **Combo or sequence** — the buttons:
  - A **combo** is two or more buttons held together (for example L3 + R3 + Square).
  - A **sequence** is buttons pressed in order (for example Up, Up, Down).
  - Any button works except **PS** (it opens the system menu). L1/R1, L2/R2, L3/R3, the face buttons, the d-pad and the touchpad are all fair game.
- **Confirmation** — what happens when it fires: **Light bar**, **Notification**, **Rumble**, or **All**.

Pick combos a game won't use by accident — holding L3 + R3 and a face button or a direction for a moment is a good pattern.

### Rumble feel

Under the master switch on the Shortcuts screen there are three rumble settings (left/right to adjust): **strength**, **length**, and **pulses** (1–3). A quick double or triple pulse stands out from a game's own rumble better than one long buzz.

## Running the reader

The in-app half only stores the shortcuts. The reader payload is what listens while you play.

1. Set your shortcuts up in Home5 **with no game open** (see the note below).
2. Load `PPSA11019-reader.elf` with your usual ELF loader.
3. You'll get a notification that the shortcuts are listening. Play — your combos now fire their actions.

The reader injects a small thread into the system UI to read the controller (a plain payload can't read the pad while a game owns it). It reads the pad without taking it from the game, so your inputs still reach the game.

### Set-up order matters

When a game launches with Home5 open, ShadowMountPlus unmounts Home5's data, and Home5 can't always save a shortcut change for the reader. **Set shortcuts up with no game open, then launch the game.** Recent builds also write the config to a shared location so mid-game edits can still reach the reader, but if a change doesn't take, close and reopen Home5.

## Starting the reader at boot (optional)

So the shortcuts are there from power-on without loading the reader by hand, add it to a payload autoloader. With [ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager):

1. Have the payload manager itself start at boot — your boot autoloader (Y2JB / BD-JB / Lua) loading `pldmgr.elf`, as usual.
2. In the manager's **Automated Startup** list, add the full path `/data/homebrew/PPSA11019/PPSA11019-reader.elf`. The manager accepts a full path, so there's nothing to copy.
3. Optionally set an autoload delay so you can abort with the PS button.
4. Reboot. The autoloader starts the manager, the manager starts the reader, the reader waits for the system UI and injects — and your shortcuts are live from boot.

Any autoloader works the same way: point it at `/data/homebrew/PPSA11019/PPSA11019-reader.elf`.

## Good to know

- **Light bar** only shows on the Home5/home screen, not over a game. The call succeeds, but whatever app is in front owns the controller's colour. Use Notification or Rumble for in-game confirmation.
- **Rumble** is the opposite — you feel it with a game in front, not on the home screen.
- Shortcuts also fire while you're using Home5 itself, which is handy. A **sequence made only of d-pad buttons** can fire just from moving through menus, so prefer a combo, or include a non-d-pad button.
- The reader can't be unloaded until a reboot (a system-UI thread can't exit); it can be stopped/parked. Loading the reader a second time is refused so actions don't fire twice.

## Credits

The background reader is built on EnriqueWood's proof of concept (GPL-3.0-or-later), with the system-UI injector from the PS5 DualSense overlay (derived from Common FPS for PS5) and ptrace/ELF-loader helpers from shsrv.
