# Home5 background shortcuts — the reader

This is the part of Home5's shortcuts that runs **outside the app**: a PS5 payload that reads the
DualSense while a game is in front and fires the shortcuts you set up in Home5.

A plain payload cannot read the controller while a game owns it (`scePadOpen` returns `0x809b0081`).
The reader works the way the PS5 DualSense overlay does: it injects a small thread into the system's
`SceShellUI`, where the controller is always readable, and that thread recognises your combos and
sequences. Nothing is drawn or hooked — one thread, one pad handle, one log.

- `h5r_thread.c` — the thread injected into `SceShellUI`. It reads the shortcuts Home5 wrote to
  `/data/homebrew/PPSA11019/shortcuts.cfg`, detects each combo or sequence, writes `FIRE <n>`, and
  flashes the light bar when a shortcut asks for it. It never returns; a stop file parks it until the
  next reboot.
- `h5r_loader.cpp` — the payload you load. It injects the thread (embedded), follows its log, and when a
  shortcut fires it calls Home Assistant over its REST API and shows a console notification.
- `h5r_net.c` — the console's network, so the reader can reuse Home5's own Home Assistant client
  (`src/h5_net.c`) for the HTTPS call.
- `build.sh`, `pins.sh` — the build. The injector is used **unchanged** from the pinned overlay/shsrv.

## Run

1. Set up Home Assistant in the Home5 app and create some shortcuts. The app writes the reader's config
   and your sign-in to `/data/homebrew/PPSA11019/`.
2. Load `reader.elf` with your ELF loader. It injects the thread and listens (up to two hours per load).
   To stop it, create `/data/homebrew/PPSA11019/stop`; to stop and park the thread, create `park`.

It will not run alongside the DualSense overlay, Common FPS or SimpleFPS: only one `SceShellUI` injector
at a time.

## Credits

This is built on **EnriqueWood's proof of concept** for Home5's background shortcuts
(GPL-3.0-or-later), which first showed the combo could be read while a game runs. The `SceShellUI`
injector is from the **PS5 DualSense overlay** (erickdavestech), derived from **Common FPS for PS5**
(porhe911); the ptrace and ELF-loader helpers are from **shsrv** (John Törnblom). All GPL-3.0-or-later.
