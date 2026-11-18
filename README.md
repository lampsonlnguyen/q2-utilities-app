# Utilities

A full-screen utility card app for the Shanling Q2 running [Q2 Pod](https://github.com/DiamondBond/q2-pod).

| Tool | Features |
| --- | --- |
| Stopwatch | Start, pause, resume, and reset. Records cumulative lap times and shows the latest four. Timing continues when you return to the Utilities menu while the app is open. |
| Calendar | Browse months, highlight the player's current date, and jump back with **Today**. The date comes from the Q2 clock. |
| To-do | Add, complete, reopen, and delete up to 32 tasks. Use the on-screen keyboard for names up to 24 characters. Tasks are saved in `.utilities/tasks.tsv` on the SD card. |
| Calculator | Add, subtract, multiply, and divide with decimal input. Operations are evaluated in entry order. **C** clears the calculator and **<** erases the last digit. |

Turn the clickwheel to move the highlight and press Centre to select. You can also tap a button. Press Return to go back, or press it from the Utilities menu to exit. Q2 Pod also handles holding Return to close the app. Stopwatch and calculator state reset when the app closes. Q2 Pod may close an idle card app after two minutes unless Smart Awake is enabled.

## Build and install

On Ubuntu or WSL, install the build tools and create a sysroot from the
original Shanling **Q2 Firmware V1.36 ZIP**.

```sh
sudo apt install clang lld make binutils curl unzip squashfs-tools zip
bash tools/mksysroot.sh "Q2 Firmware V1.36.zip" "$HOME/.cache/q2/sysroot"
make
```

Use the full path to the stock ZIP if it is outside the checkout. If you already
have a Q2 sysroot, run `make SYSROOT=/actual/sysroot/location`.

1. Extract `dist/utilities.zip` at the root of the SD card. The result should be `.utilities/q2-utilities` and `.utilities/q2app`. The leading dot may hide the folder in your file browser.
2. Eject the card and open **Library → Utilities**, above Games, in a Q2 Pod build containing [Library card-app placement](https://github.com/DiamondBond/q2-pod/commit/41b18ffa1979acaeeb98f49b7dd921d7d321b1ce).

The manifest uses `name=Utilities`, `game=1`, and `row=library`. The `game` key enables full-screen rendering; the `row` key places the launcher directly in Library. Older firmware that does not support `row` continues to list Utilities under Games.

For an existing installation, replace `.utilities/q2app` with the updated file and leave and reopen Library. This placement change does not require rebuilding the executable.

`make check` runs the host checks with a C compiler. It exercises the on-screen touch targets, wheel and Centre navigation, lap and pause timing, a leap-day calendar, to-do persistence, and calculator error recovery.

## Credits

[Buddy](https://github.com/DiamondBond/q2-fidget) and [Groovebox](https://github.com/DiamondBond/q2-groovebox) by Diamond Bond are the reference implementations for the Q2 Pod manifest, framebuffer, and input socket. The Q2 startup code is adapted from the Rockbox startup file distributed by Groovebox (GPL-2.0-or-later). The Liberation Sans Bold bitmap in `src/font.h` comes from Groovebox and is covered by the [SIL Open Font License](LICENSES/OFL-1.1.txt).

## Appearance

Utilities follows Q2 Pod’s saved Minimal, Modern, or Classic theme, including each theme’s accent and Light Mode. It reads `/mnt/data/config.ini` when opened; reopen Utilities after changing your theme. Auto Light Mode uses the device’s local clock (light from 07:00 to 19:00) and updates while the app is open. The interface uses lightweight flat fills rather than loading firmware theme artwork. Missing settings default to Minimal, Graphite, and dark mode.

The bottom of the screen is kept clear, without footer text.

Menu rows are compact (38-pixel spacing) to leave room for additional entries. Interface text uses smooth glyphs close to the original size, with centered headings and safe screen margins.

## Automatic builds and releases

GitHub Actions tests and builds each push to `main` and every pull request. Download the **q2-utilities-app-card-app** artifact from a successful run, or start a build with **Actions → q2-utilities-app → Run workflow**.

Push an unused version tag to publish a release automatically:

```sh
git pull --ff-only
git tag v1.0.1
git push origin v1.0.1
```

Once the build and package checks pass, Actions creates the release and attaches **utilities.zip** directly. Re-running the tag build replaces that release's ZIP. Branch pushes and manual branch builds only save Actions artifacts.

CI downloads the official Shanling V1.36 ZIP from its published Google Drive link, verifies the SHA-256 in `tools/mksysroot.sh`, and caches the extracted build sysroot. Firmware files are not included in the release ZIP.
