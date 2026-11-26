# RAC1 HD Achievements

RAC1 HD Achievements tracks all 35 Ratchet & Clank HD trophies in OpenRAC. Load an existing save to credit trophies the save can prove. New unlocks during play show a tier-colored popup with a short chime. Progress stays local.

The collection is available in **Mods → RAC1 HD Achievements → Open**. It shows the trophy requirements, earned status, and progress for goals with measurable counts.

The mod supports Windows and Linux x86-64 with Ratchet & Clank PAL (`SCES_509.16`) or NTSC-U (`SCUS_971.99`). Tracking uses OpenRAC mod API 1.11 or later. Popups require API 1.14 or later. The collection page was tested with OpenRAC Launcher 1.1.0.

## Install

When a release is available, select Ratchet & Clank in OpenRAC Launcher. Open **Mods**, choose **Add from a link**, and paste the repository address.

```text
https://github.com/Joebannerlord/openRAC-RC1-PS3-HD-Achievements
```

Choose **Install**, enable the mod, then start the game. You can also download the ZIP for your platform from [Releases](https://github.com/Joebannerlord/openRAC-RC1-PS3-HD-Achievements/releases) and select the ZIP through **Mods → Add a mod**. See the [OpenRAC installation guide](https://openrac.dev/mods/docs/install-from-link) for details.

Load a save, then open or refresh the collection page to see your progress. Press **Insert** during play to preview the Vandal popup without awarding the trophy. You can change the preview key or mute the chime in the mod settings.

## Build

On Windows, use an x64 Visual Studio developer prompt with MSVC. On Linux x86-64, use GCC or Clang. Both need CMake 3.20 or newer and Python 3. The SDK fetcher downloads and verifies the version in [`sdk.lock`](sdk.lock).

```powershell
python tools/fetch_sdk.py
cmake -S . -B build
cmake --build build --config Release --target release_zip
```

The SDK-generated platform ZIP goes into `build/dist/`.

On Linux, popup text uses the game overlay font and the built-in tier medals.

Licensed under [GPL-3.0-or-later](LICENSE). The [OpenRAC Mod SDK notice](LICENSE-OPENRAC-SDK) and [Audiowide font license](ui/LICENSE-Audiowide.txt) are included.
