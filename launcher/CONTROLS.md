# Controls

Port 0 accepts the keyboard, the mouse and the first connected gamepad at the same time. Port 1 accepts the second gamepad only. Press F11 to toggle fullscreen. Closing the window exits.

## Keyboard and mouse

Click in the window to capture the mouse (that click does not fire). While captured, mouse movement turns the camera and the mouse buttons fire. Escape (which also pauses) or switching to another window releases it.

| Action | PS2 input | Keyboard / mouse |
|---|---|---|
| Move | Left stick | W / A / S / D |
| Camera / aim | Right stick | Mouse movement |
| Primary weapon | R1 | Left mouse button (also 2) |
| Secondary weapon | R2 | Right mouse button (also 3) |
| Mini-Con slots | L1 / L2 | Q or Left Shift or mouse back button / E or Left Ctrl or mouse forward button (also 1 for L2) |
| Jump, menu confirm | Cross | Space (also K) |
| Transform | Triangle | F (also I) |
| | Square | R (also J) |
| | Circle | C (also L) |
| | L3 / R3 | V / Z or middle mouse button |
| Pause | Start | Escape or Enter |
| | Select | Tab or Backspace |
| Menu navigation | D-pad | Arrow keys |

Keyboard stick directions are full deflection.

Mouse look on foot turns the game camera directly: the game's camera controller is hooked (`CAMERA_HOOKS` in `tools/recomp_ee.py`), and the mouse motion is added to the camera yaw and pitch 1:1, without the stick's turn-rate cap, dead zone or ramp. One count turns 0.0025 rad (about 0.14°) at `--mouse-sens 1`. Pitch keeps the game's own limits. The game levels the camera pitch while you run without touching the stick; that is suppressed for one second after the mouse last moved. Up/down follows the game's "Invert Y axis" option, and `--invert-mouse` flips it again.

Where the hooked controller is not running (vehicle mode, other camera modes), the mouse falls back to driving the right stick at a speed that follows the mouse speed, as before. `--mouse-stick` always uses that stick emulation.

## Gamepad

| PS2 input | SDL gamepad |
|---|---|
| D-pad | D-pad |
| Cross / Circle / Square / Triangle | A / B / X / Y (south / east / west / north) |
| L1 / R1 | Left / right shoulder |
| L2 / R2 | Left / right trigger (above 30%) |
| Start / Select | Start / Back |
| L3 / R3 | Left / right stick click |
| Left / right stick | Left / right stick |

Buttons pressed on any device are combined. A held keyboard stick direction or a moving captured mouse takes precedence over that gamepad stick axis. A disconnected port 1 reports no pressed buttons and centered sticks.
