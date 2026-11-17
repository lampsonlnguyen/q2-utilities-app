# Q2 Utilities

A full-screen utility card app for the Shanling Q2 running [Q2 Pod](https://github.com/DiamondBond/q2-pod).

| Tool | Features |
| --- | --- |
| Stopwatch | Start, pause, resume, and reset. Records cumulative lap times and shows the latest four. Timing continues when you return to the Utilities menu while the app is open. |
| Calendar | Browse months, highlight the player's current date, and jump back with **Today**. The date comes from the Q2 clock. |
| To-do | Add, complete, reopen, and delete up to 32 tasks. Use the on-screen keyboard for names up to 24 characters. Tasks are saved in `.utilities/tasks.tsv` on the SD card. |
| Calculator | Add, subtract, multiply, and divide with decimal input. Operations are evaluated in entry order. **C** clears the calculator and **<** erases the last digit. |

Turn the clickwheel to move the highlight and press Centre to select. You can also tap a button. Press Return to go back, or press it from the Utilities menu to exit. Q2 Pod also handles holding Return to close the app. Stopwatch and calculator state reset when the app closes. Q2 Pod may close an idle card app after two minutes unless Smart Awake is enabled.

## Build and install

You need Clang, LLD, Make, zip, and the Shanling Q2 sysroot. Use [Rockbox's sysroot script](https://github.com/DiamondBond/q2-rockbox/blob/main/tools/shanlingq2/mksysroot.sh) with stock firmware **V1.32** to create it.

```sh
make SYSROOT=/path/to/q2-sysroot
```

1. Extract `dist/utilities.zip` at the root of the SD card. The result should be `.utilities/q2-utilities` and `.utilities/q2app`. The leading dot may hide the folder in your file browser.
2. Eject the card and open **Library → Games → Utilities** in a Q2 Pod build with card game support.

`make check` runs the host checks with a C compiler. It exercises the on-screen touch targets, wheel and Centre navigation, lap and pause timing, a leap-day calendar, to-do persistence, and calculator error recovery.

## Credits

[Buddy](https://github.com/DiamondBond/q2-fidget) and [Groovebox](https://github.com/DiamondBond/q2-groovebox) by Diamond Bond are the reference implementations for the Q2 Pod manifest, framebuffer, and input socket. The Q2 startup code is adapted from the Rockbox startup file distributed by Groovebox (GPL-2.0-or-later). The Liberation Sans Bold bitmap in `src/font.h` comes from Groovebox and is covered by the [SIL Open Font License](LICENSES/OFL-1.1.txt).
