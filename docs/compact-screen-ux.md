# Compact-screen utility UI design (Q2 Pod)

## Problem in v1.4.0

The eight new tools reused one generic layout of three 20px text lines over
up to seven equal-weight 110px buttons. In a 375x320 Q2 layout, that made
the result hard to see, forced the eye to scan all buttons, and placed
short-lived status messages alongside the key number.

## External design references reviewed

- **Fossify Clock:** https://github.com/FossifyOrg/Clock — large countdown,
  preset/secondary actions beneath the running timer, distinct Start action.
- **Tack Metronome Android / Wear OS:** https://github.com/patzly/tack-android
  — BPM-centric interface, beat indicators, controlled +/- adjustments.
- **Unitto:** https://github.com/sadellie/unitto — clear input and result
  values, unit labels adjacent to the value, unobtrusive swap action.
- **Fossify Calculator:** https://www.fossify.org/apps/ — paired unit values.
- **Android Wear OS UI guidance:**
  https://developer.android.com/design/ui/wear/guides/get-started/design-for-wearables
  — glanceable values and actions prioritized for reduced viewport.

These are visual interaction references, not imported Android code or assets.
The implementation remains the native Q2 Pod card app in C, with its existing
system-theme palette and original font/icon glyphs.

## Result

Screen parts at 375x320:

- System-theme title bar: y=0..25 (unchanged).
- Glanceable information/hero: y=28..185. Large, single readout and
  mode/status; a bounded progress bar or four counter channels where useful.
- Primary controls: y=190..235, a 46px-tall action row.
- Secondary controls: y=250..291, a 42px-tall adjustment row.
- Blank bottom edge: y=292..319; Return key handles Back.

Per-tool layouts:

| Tool | Primary hierarchy | Adjustments |
|---|---|---|
| Pomodoro | Focus/break + large remaining time + progress + session count | Start, reset, switch work/break; work/rest minutes |
| Countdown | Large remaining time + progress | Start, reset; +/-1 minute or 10 seconds |
| Metronome | BPM number + beat indicators | Start, beats, tap; +/-1 and +/-10 BPM |
| Converter | Source value over destination value, divided by thin rule | Units, swap; +/-1 and +/-10 |
| Random | Large generated result + mode/range | Generate, mode; min/max |
| Date | ISO date + offset from local today | Today; +/-1 and +/-30 days |
| Engineering | Formula result + parameters + editing field | Mode, select field; fine/coarse changes |
| Counter | Selected counter with large count + four channels | Increment, decrement, reset, next/previous |

Buttons preserve the previous action codes and engine semantics; only
their layout and visual hierarchy change. All controls remain wheel/Center/
touch accessible. High-contrast text inherits Q2 Pod's Minimal, Classic
and Modern theme settings. New assets and Android dependencies: **none**.

## Limitations

- Metronome is still visual-only (audio API not yet integrated).
- Pomodoro and Countdown remain bound to the app lifecycle; Q2 Pod may
  close idle card apps. No false promise of notifications after exit.
- No native Android app framework or Kotlin/Compose dependencies.
- Physical Q2 touch and long-session battery testing remains desirable;
  GitHub host/MIPS tests do not prove device ergonomics.
