# CachyOS reference setup

The measured reference uses the native CachyOS OBS package and a
kernel-matched `v4l2loopback` module. Because CachyOS kernels update frequently,
confirm that the module was built for the currently booted kernel before each
test. A present `/dev/video42` is not enough: `openlens doctor` and the
synthetic test must both pass.

```sh
openlens synthetic --video /dev/video42 --seconds 10
```

Use the native OBS package for the certified path. Flatpak OBS may not see the
host Avahi service, OpenLens identity directory, plugin directory, or V4L2
device without additional portal and permission work; it is not currently a
claimed supported package.
