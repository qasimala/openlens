# Linux setup

Install FFmpeg runtime/development libraries, OpenSSL 3, Avahi, a C++20
toolchain, CMake/Ninja, Qt 6.5+, OBS, and `v4l2loopback` through your
distribution. Ensure the Avahi daemon is running and local multicast DNS is not
blocked by the firewall. OpenLens does not need Android Platform Tools or ADB.

For an ephemeral V4L2 reference setup, review and run this yourself:

```sh
sudo modprobe v4l2loopback video_nr=42 card_label=OpenLens exclusive_caps=1
```

Open the Android and desktop apps on the same Wi-Fi, pair once, and use the
desktop **Start camera** button. The optional CLI equivalents are:

```sh
openlens doctor
openlens devices
openlens pair
openlens start --video /dev/video42
```

If several phones are available, select one in the desktop app or pass its
non-hardware installation ID using `--id`. Module persistence and DKMS/kernel
package choices are distribution-specific and left to the system owner.
