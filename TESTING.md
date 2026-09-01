# Testing checklist

Test on retail dashboard 2.0.17559.0 with the plugin configured through
DashLaunch. Boot the dashboard with the controller disconnected, then connect
it by USB after the dashboard is fully loaded.

## Core input

- [ ] D-pad: all four directions and diagonals.
- [ ] Face buttons: A, B, X, and Y.
- [ ] Menu/Start and View/Back.
- [ ] LB/RB and L3/R3.
- [ ] Left and right triggers, including partial analogue travel.
- [ ] Left and right sticks, including full range and click actions.
- [ ] Guide: a brief press opens/closes the Xbox Guide once per press.

## System integration

- [ ] Controller is assigned a player slot and ring-of-light quadrant.
- [ ] Dashboard navigation works.
- [ ] Launch a retail game and confirm input works in-game.
- [ ] Open the Guide over a game; Guide navigation must not also control the
  game behind it.
- [ ] Game rumble drives the controller’s left and right motors.

## Reliability

- [ ] Disconnect and reconnect after the dashboard has loaded.
- [ ] Let the controller sleep, wake it, then confirm input recovers.
- [ ] Reboot with the plugin enabled and controller unplugged.
- [ ] Reboot with another physical Xbox 360 controller connected, if available.

## Controller coverage

Record the exact controller model and USB product ID for every test. The
current allow-list covers official Microsoft vendor ID `045E` with these
product IDs:

| Product ID | Expected family |
| --- | --- |
| `02D1` | Xbox One |
| `02DD` | Xbox One (2015 revision) |
| `02E3` | Xbox Elite |
| `02EA` | Xbox One S |
| `0B00` | Xbox Elite Series 2 |
| `0B12` | Xbox Series X|S wired |

## Known limitations

- One Xbox One/Series GIP controller is supported at a time.
- Guide tap works. The held-Guide native power menu is not exposed through the
  retail 17559 virtual-controller API.
- Headset/audio and Share have no Xbox 360 controller equivalent.
- Wireless adapters and non-controller Microsoft GIP accessories are excluded.
