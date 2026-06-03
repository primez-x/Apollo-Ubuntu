# Apollo Ubuntu Flatpak

The Flatpak manifest is maintained for experimentation and future distribution of Apollo Ubuntu.
The `.deb` package remains the recommended Ubuntu install path.

## Host Prerequisites

```bash
sudo apt update
sudo apt install flatpak flatpak-builder
```

## Build

Configure the manifest from the repository root:

```bash
cmake -B build-flatpak-manifest -S . \
  -DSUNSHINE_CONFIGURE_ONLY=ON \
  -DSUNSHINE_CONFIGURE_FLATPAK_MAN=ON \
  -DSUNSHINE_BUILD_FLATPAK=ON
```

If `package-lock.json` changes, refresh the checked-in npm source list before
configuring the manifest:

```bash
python3 packaging/linux/flatpak/deps/flatpak-builder-tools/npm/flatpak-npm-generator.py \
  package-lock.json -o generated-sources.json
```

Build the Flatpak:

```bash
flatpak-builder --force-clean --repo=repo flatpak-build build-flatpak-manifest/io.github.primezx.ApolloUbuntu.yml
flatpak build-bundle repo ApolloUbuntu-x86_64.flatpak io.github.primezx.ApolloUbuntu
```

## Install

```bash
flatpak install --user ./ApolloUbuntu-x86_64.flatpak
flatpak run --command=additional-install.sh io.github.primezx.ApolloUbuntu
flatpak run io.github.primezx.ApolloUbuntu
```

`additional-install.sh` installs the user service, host input rules, and module-load file through `flatpak-spawn --host pkexec`.

## Limitations

- Validate GNOME Wayland, PipeWire capture, and Moonlight streaming before publishing a Flatpak build.
