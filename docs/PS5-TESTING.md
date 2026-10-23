# Testing Home5 on a PS5

The host tests (`tests/test.sh`) run everything but the console's own parts against a fake
Home Assistant. These are the parts only a PS5 can show. Before a release, go through them
on the console and note the firmware and loader.

Put an empty `home5-log.txt` at the top of a USB drive first: Home5 then writes its log
there (it starts with `H5 ps5 Home5` and the version).

| # | Check | Expect |
| --- | --- | --- |
| 1 | Install `PPSA11019`, look at Home | the tile **Home5** under Media, the house icon; the dark background with the house when selected |
| 2 | Start it | the sign-in screen, sharp, colours right (blue button, not orange) |
| 3 | D-pad, left stick, ✕ on Address | moving works; the PS5's keyboard opens over the screen, typing gives the text back |
| 4 | Password | typed hidden; ○ cancels and changes nothing |
| 5 | Sign in with `http://<IP>:8123` | "Connecting…", then "Make your first room" (or the rooms made before) |
| 6 | A two-step account | the code screen, the number keyboard |
| 6b | Options › New room, a name, then the list: L2 / R2 through the kinds, ✕ on a few | they are in the room when you press ○ |
| 6c | In the list, △, type part of a name (two words), then □ | the keyboard has no capitals at the start; only what matches, "Searching for …" at the top; □ shows everything again |
| 6d | A room with nothing in it | no card in it: "Nothing in this room yet"; Options › Add to the room opens the list |
| 7 | ✕ on a light | it switches at once in Home5 and in the house |
| 8 | Hold R2 / L2 on a light | brightness steps up / down while held; the triggers push back |
| 9 | △ on a light, left / right, □ | its screen; the colour changes; the light bar follows the light |
| 10 | A thermostat: L2 / R2, modes | the target changes; the mode changes in Home Assistant |
| 11 | Menu: add, rename, new room, move left / right; a scene's card | kept after closing and starting Home5 again; nothing changed in Home Assistant; the scene runs |
| 12 | Up to the room tabs, □ on one | asked first; ○ keeps it, ✕ deletes it |
| 13 | A Home Assistant notification (`persistent_notification.create`) | the console's notification |
| 14 | Turn Home Assistant off, then on | "Not connected", then back by itself |
| 15 | `home5.txt` with a long-lived token | signed in without typing |
| 16 | An `https://` address with a public certificate | signs in; a self-signed one is refused with a clear message |
| 17 | Sign out | back to the sign-in screen; the token gone from Home Assistant's profile |
| 18 | Leave it open 30 minutes | still connected (tokens are renewed), no slowdown |

If something fails, keep the log and the step's number.
