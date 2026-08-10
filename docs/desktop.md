# OpenLens Desktop

OpenLens Desktop is the normal way to use the project. No terminal, cable, IP
address, account, or ADB connection is required after the apps are installed.

## First use

1. Put the phone and PC on the same trusted local Wi-Fi.
2. Open OpenLens on the phone, allow camera access, and leave it open.
3. Open OpenLens on the PC. The phone appears automatically.
4. Select it and click **Pair phone**.
5. Compare the large six-digit code on both screens. If it matches, click
   **Codes match** on the PC and phone. Cancel if it differs.
6. Pairing is remembered on both devices. Click **Start camera** on the PC.
7. In OBS, add **Video Capture Device (V4L2)** and select OpenLens, or install
   and add the native **OpenLens Phone Camera** source.

## Later use

Open both apps on the same network and click **Start camera** on the PC. The
phone does not ask again. Camera privacy remains visible through Android's
indicator and foreground notification, and the phone notification always has
an emergency Stop action.

If either app is reinstalled or its identity changes, use **Forget paired
computer** on the phone and `openlens forget --id …` on the PC, then pair again.
OpenLens never accepts a changed identity silently.

Settings are remembered for the current Linux user. If Wi-Fi drops, the
desktop shows a disconnect slate and searches for the same paired phone for up
to 60 seconds.

## Installation

After a development build, `scripts/install-desktop-app.sh` installs the
desktop binary, bundled OBS module, application-menu entry, and icon under the
current user's `~/.local` directories. It does not use administrator
privileges. The matching uninstall script removes only OpenLens-owned files.
