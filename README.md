# Utilities

A full-screen utility card app for the Shanling Q2 running [Q2 Pod](https://github.com/DiamondBond/q2-pod).

| Tool | Features |
| --- | --- |
| Stopwatch | Start, pause, resume, and reset. Records cumulative lap times and shows the latest four. Timing continues when you return to the Utilities menu while the app is open. |
| World Clock | Local time in 16 preset cities, each with its Casio-style 3-letter code, date, UTC offset, daylight-saving state, and day/night. Select a city for a detail view with its full date, offset, and difference from Los Angeles. A **World map** button at the bottom of the list opens a tiny world map with GMT bands: turn the wheel to move the pin west to east, or tap the map, and press Centre to open that city's detail view. |
| Calendar | Browse months, highlight the player's current date, and jump back with **Today**. The date comes from the Q2 clock. |
| To-do | Add, complete, reopen, and delete up to 32 tasks. Use the on-screen keyboard for names up to 24 characters. Tasks are saved in `.utilities-app/tasks.tsv` on the SD card. |
| Calculator | Add, subtract, multiply, and divide with decimal input. Operations are evaluated in entry order. **C** clears the calculator and **<** erases the last digit. |
| Pomodoro | Adjustable work/break intervals, session count, monotonic running/paused countdown. |
| Metronome | Classic Wittner-style pyramid with a swinging pendulum (visual beat) at 30–300 BPM, tap tempo, and 3/4, 4/4 or 6/8 display. The pendulum's sliding weight sits on the 40–208 BPM tempo scale on the face. Sound is not yet integrated: the card app must not seize the Q2 music audio device. |
| Unit Converter | Offline km/mi, kg/lb, Celsius/Fahrenheit, km/h/mph, cm/in, and MiB/MB conversions, with adjustable inputs and swap. |
| Random Generator | Random integer in adjustable range, six-sided die and coin flip. Not suitable for security/cryptography. |
| Countdown Timer | Editable timer, start/pause/reset, minute and 10-second increments. |
| Date Calculator | Add/subtract days from the Q2's local date, correctly handling leap years. |
| Engineering Calculator | Ohm's law (V, P from R/I), voltage divider, and ideal battery life; editable parameters. |
| Event Counter | Four independent tally channels, increment/decrement/reset, saved atomically to `.utilities-app/counters.tsv` on SD. |

## New tools and navigation

The home screen currently displays only the five established utilities:
Stopwatch, World Clock, Calendar, To-do, and Calculator. The **Clocks & Timers**
and **Tools & Calculators** menus (and their eight tools) are hidden pending UI
redesign. Their source code, tests, and saved data formats remain intact.
The home screen and both submenus have tiny 12×12 bitmap-font icons: glyphs
are compiled into the executable and inherit the selected Q2 theme's colors
in all Minimal, Modern, and Classic variants. No PNG or font files are needed.
Turn the Q2 wheel to select an item or action, press Centre, or tap an item.
Return goes back one level. The new screens never ask for a keyboard.

**Timer and metronome scope:** while Utilities is open, Pomodoro and Countdown
use the Q2 monotonic clock (not frame counting); leaving their screens does
not stop them. They do not continue after the Q2 firmware terminates the app;
enable Q2 Pod Smart Awake for sessions longer than the firmware's idle limit.
The metronome is currently *visual only*. It deliberately does not open the
audio device or compete with Q2 Pod music playback; an audible beat requires
a verified system-level audio/mixing interface.

The date calculator is based on the Q2's *local date*. Conversion and
engineering calculators use approximate decimal values. Event Counter is
saved after every increment/decrement/reset; a storage failure is shown in
the counter screen. To preserve counters during upgrades, keep
`.utilities-app/counters.tsv`.

## World Clock

World Clock lists 16 cities, in the style of Ubuntu's GNOME Clocks list
(city, local time, date, UTC offset) with the Casio AE1200's 3-letter city
codes (LAX, NYC, TYO, …) and its 31-time-zone / 48-city idea cut down to a
scrollable preset: Los Angeles, New York, Chicago, Denver, Honolulu, London,
Paris, Berlin, Moscow, Dubai, Mumbai, Ho Chi Minh City, Singapore, Tokyo,
Sydney, Auckland. Times are 24-hour. Day means 06:00–18:00 local, Night
otherwise, and a `+1d` / `-1d` mark shows when a city's date differs from
the Q2's local date.

Time zones are resolved from an embedded table in `src/worldclock.c`, not
from the device: the Q2's stock firmware ships no timezone database
(`/usr/share/zoneinfo` is absent from the V1.36 sysroot), so each city
carries its IANA base offset plus its daylight-saving rule (US 2nd Sunday
in March / 1st Sunday in November, EU last Sunday in March / October,
Sydney and Auckland October–April). The rules were checked against IANA
tzdata for 2026, including the spring-forward and fall-back instants and
the southern hemisphere's reversed seasons; `tests/test_worldclock.c`
pins those transitions. If a government changes a rule, the table needs
updating — the app cannot learn it from the device. Adding or removing
cities is a one-line change to the `WC_CITIES` table in the same file.

Turn the clickwheel to move the highlight and press Centre to select. Utilities uses the **Wheel Sensitivity** from Q2 Pod’s **System settings → Controls** menu. You can also tap a button. Press Return to go back, or press it from the Utilities menu to exit. Returning from a city's detail view lands back on that city in the list. Q2 Pod also handles holding Return to close the app. Stopwatch and calculator state reset when the app closes. Q2 Pod may close an idle card app after two minutes unless Smart Awake is enabled.

### World map picker

The **World map** button at the bottom of the World Clock list opens a
picker in the style of Ubuntu's Date & Time settings: a tiny world map
with a vertical line for every hour of longitude and a GMT scale below
it (-12 to +12), the pin city's band highlighted, and an info panel
underneath showing the city and country, its abbreviation and UTC
offset with the local time, and the date with its difference from Los
Angeles — the same tooltip Ubuntu shows over its map ("PDT (UTC-07),
Los Angeles, United States, 03:41").

Turn the wheel to move the pin from city to city, west to east, or tap
the map to jump to the nearest city (tapping the GMT scale picks by
longitude alone). Press Centre, or tap the info panel, to open that
city's detail view; press Return to go back to the list, focused on
the pin's city. The map is drawn from coarse continent outlines in
`src/app.c`, not from an image file: the card app ships no assets, and
each city carries its latitude, longitude, country, and abbreviation
in `src/worldclock.c`. The Q2 is a colour screen, so each landmass gets
its own soft atlas colour (teal North America, green South America,
lavender Europe, sandy Africa, rose Asia, ochre Australia, and so on),
one fixed mid-tone palette that reads on both the light and the dark
paper, and the pin city's GMT band is tinted with the theme accent.

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

1. Extract `dist/.utilities-app.zip` at the root of the SD card. The result should be `.utilities-app/q2-utilities-app` and `.utilities-app/q2app`. The leading dot may hide the folder in your file browser.
2. Eject the card and open **Library → Utilities**, above Games, in a Q2 Pod build containing [Library card-app placement](https://github.com/DiamondBond/q2-pod/commit/41b18ffa1979acaeeb98f49b7dd921d7d321b1ce).

The manifest uses `name=Utilities`, `game=1`, and `row=library`. The `game` key enables full-screen rendering; the `row` key places the launcher directly in Library. Older firmware that does not support `row` continues to list Utilities under Games.

For an existing installation, install the new ZIP, copy `.utilities/tasks.tsv` to `.utilities-app/tasks.tsv` to preserve your tasks, and remove the old `.utilities` folder to avoid a duplicate launcher. Leave and reopen Library.

The store derives the app ID `utilities-app` from this repository name. The ZIP must contain `.utilities-app/q2app` and `.utilities-app/q2-utilities-app`; the executable listens on `/tmp/q2-utilities-app.input.sock`.

`make check` runs the host checks with a C compiler. It exercises the on-screen touch targets, wheel and Centre navigation, lap and pause timing, a leap-day calendar, to-do persistence, calculator error recovery, and World Clock navigation plus its DST transitions (US/EU spring-forward and fall-back, Sydney/Auckland reversed seasons, leap-day rollover, half-hour Mumbai) and the world map picker (longitude order, nearest-city taps including across the date line, wheel to Tokyo, Centre to its detail view, Return back to the list).

## Credits

[Buddy](https://github.com/DiamondBond/q2-fidget) and [Groovebox](https://github.com/DiamondBond/q2-groovebox) by Diamond Bond are the reference implementations for the Q2 Pod manifest, framebuffer, and input socket. The Q2 startup code is adapted from the Rockbox startup file distributed by Groovebox (GPL-2.0-or-later). The Liberation Sans Bold bitmap in `src/font.h` comes from Groovebox and is covered by the [SIL Open Font License](LICENSES/OFL-1.1.txt). The q2-pod scheme data in `src/q2pod_scheme.c` — the ACCENTS tones from `patch/offsets.inc`, Classic Blue's gloss band and status bar from `assets/themes/classic-light/theme.xml`, and the gloss derivation from `tools/theme.py` — is ported from DiamondBond's [Q2 Pod](https://github.com/DiamondBond/q2-pod) and was first adapted for q2-weather; see that file's header for the sources.

## Appearance

Utilities follows Q2 Pod’s saved Minimal, Modern, or Classic theme, including each theme’s accent and Light Mode. It reads `/mnt/data/config.ini` when opened; reopen Utilities after changing your theme. Auto Light Mode uses the device’s local clock (light from 07:00 to 19:00) and updates while the app is open. The interface is drawn as q2-pod lists: a 26-pixel status bar with the centred screen title, 32-pixel rows from below the bar, the theme's own selection painter (Minimal card, Classic gloss band, Modern accent bar), and a chevron on the selected row. There is no on-screen back label: the wheel's Return key goes back. The Stopwatch, Calendar, To-do, and Calculator pages use the same bar and row painting; the calendar's month and year move into the title bar and today is marked with the selection. World Clock uses 52-pixel two-line rows instead of 32-pixel rows so each city fits its time, date, offset, and day/night, with the same bar, selection painter, and chevron, plus a World map button in the 34-pixel strip below the rows. No theme artwork or image files are loaded. Missing settings default to Minimal, Graphite, and dark mode.

## Automatic builds and releases

GitHub Actions tests and builds each push to `main` and every pull request. Download the **q2-utilities-app-card-app** artifact from a successful run, or start a build with **Actions → q2-utilities-app → Run workflow**.

Push an unused version tag to publish a release automatically:

```sh
git pull --ff-only
git tag v1.0.1
git push origin v1.0.1
```

Once the build and package checks pass, Actions creates the release and attaches **.utilities-app.zip** directly. Re-running the tag build replaces that release's ZIP. Branch pushes and manual branch builds only save Actions artifacts.

CI downloads the official Shanling V1.36 ZIP from its published Google Drive link, verifies the SHA-256 in `tools/mksysroot.sh`, and caches the extracted build sysroot. Firmware files are not included in the release ZIP.
