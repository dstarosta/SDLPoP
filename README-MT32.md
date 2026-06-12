# Roland MT-32 / CM-32L music

SDLPoP can play Prince of Persia's MIDI music through a **real Roland MT-32 / CM-32L emulator**
(the same emulator core DOSBox uses), instead of the built-in OPL/Adlib synth. This gives you the
authentic Roland sound the game was originally scored for, using Prince of Persia's own custom
instruments.

It is completely **optional** and **automatic**: if the emulator and the required files are all
present, the music plays through the MT-32; if anything is missing, the game silently falls back to
the normal OPL/Adlib music.

To enable it you need three things:

1. The **emulator library** (libmt32emu) - included for Windows (x86 and x64).
2. Prince of Persia's **MT-32 instruments** file - included.
3. A licensed MT-32 **ROM** file pair.

Each is explained below.

---

## 1. The emulator library (libmt32emu)

The MT-32 sound is produced by Munt's `libmt32emu`. **Version 2.8 or newer is required.**

### Windows

Nothing to do -- the library is **included** with the game. Just keep the `libmt32emu-x64.dll`
(64-bit build) and/or `libmt32emu.dll` (32-bit build) file next to `prince.exe`, or in the `roms`
folder. The game loads the one matching its build automatically.

### Linux

Install `libmt32emu` (version 2.8+). It may not be in your distribution's package repositories, in
which case build it from source:

```sh
git clone https://github.com/munt/munt.git
cd munt/mt32emu
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON -Dlibmt32emu_C_INTERFACE=TRUE
make
sudo make install
sudo ldconfig
```

After installing, the game finds the library on the normal system library path.

### macOS

Install `libmt32emu` (2.8+) with Homebrew if a recent enough version is available:

```sh
brew install mt32emu
```

You may need to add `export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib"`
to the `.zshrc` (or the corresponding file for your shell) in your home `~/` folder if the game
falls back to the OPL MIDI (printed in the console).

Otherwise build it from source using the same steps as Linux (you can drop the resulting library
next to the game instead of installing it system-wide).

---

## 2. Prince of Persia's MT-32 instruments (resource -1)

Nothing to do -- the **`data/PRINCE/res-1.bin`** file is included.

On real hardware the game uploads its own custom instrument set to the MT-32 at startup -- that is
why, on a real unit, the display reads **"The Princess awaits."** when you launch the game. Without
this upload the MT-32 would play the music with its wrong, factory instruments (drums where melodies
should be), so SDLPoP treats this file as required for the MT-32 mode and falls back to OPL if it is
missing.

In Prince of Persia's data files, these instruments are stored under a slightly odd name: the game
labels them resource number **`-1`**. You don't have to find or rename anything -- SDLPoP knows where
to look. It checks two places, in order:

1. A ready-made file at **`data/PRINCE/res-1.bin`** (the `res-1` is just that resource number `-1`).
2. If that file isn't there, it reads the instruments **straight out of `PRINCE.DAT`**.

Either one works, so in practice this just happens on its own.

**One thing about game versions:** these MT-32 instruments were only added in Prince of Persia
**1.3** (the version that introduced Roland support). If your game data comes from an earlier
version (1.0–1.2), the instruments simply aren't there -- and that's fine. SDLPoP just plays the
normal OPL/Adlib music instead, which is exactly what those versions always did.

---

## 3. The ROMs

The MT-32 / CM-32L emulator needs the original Roland **ROM** files to make any sound. These are
**copyrighted by Roland and are NOT included** -- you must supply your own from hardware you own.

Other applications like Munt and DOSBox Staging have the exact same requirement.

Put the ROM pair in a folder named **`roms`** next to the game (next to `prince.exe`):

```
roms/
    cm32l_control.rom
    cm32l_pcm.rom
```

You can use either set:

* **CM-32L** (recommended): `cm32l_control.rom` + `cm32l_pcm.rom`
* **MT-32**: `mt32_control.rom` + `mt32_pcm.rom`

CM-32L is preferred in the most cases because Prince of Persia's Roland music was authored for it.
The only exception is the GENERATION1 DAC `mt32_dac` option which prefers the MT-32 ROM. The emulator
recognises the ROMs by their contents, so the exact file names only need to match one of the pairs above
(upper- or lower-case both work, but both files must use the same convention).

---

## How it's chosen

When the game starts the music for the first time it checks for all three pieces above. If the
library, a usable ROM pair, and the instruments file are all present, you'll see:

```
MT-32: Using Roland emulation for MIDI music.
```

and the music plays through the emulated Roland. If anything is missing. the game continues
normally with the OPL/Adlib music -- nothing breaks. It may produce a message if the library/dll`
are not available or it could not find the MT-32 resource -1.

---

## Adjusting the sound (optional)

A few runtime options in `SDLPoP.ini` file control the MT-32 feature:

* `mt32_dac` -- shapes the tone/character of the output, emulating hardware DAC differences.
  - `-1` -- OPL: disables the MT-32 emulator.
  -  `0` -- NICE: produces the cleanest samples, no DAC tricks.
  -  `1` -- PURE: clips samples within range, no DAC tricks; volume is normalized by SDL only.
  -  `2` -- GENERATION1: emulates the old MT-32 DAC -- warmer/softer top end. Prefers the MT-32 ROM over the CM-32L ROM.
  -  `3` -- GENERATION2: emulates the newer MT-32 / CM-32L DAC (default).

* `mt32_quality` -- how much of the Roland's analog character to emulate, shaping the tone (bass/warmth) of the music.
  - `0` -- digital: only the digital path is emulated, the fastest mode; clean but more synthetic.
  - `1` -- coarse: coarse low-pass filter emulation, boosts higher frequencies, fixed sample rate.
  - `2` -- accurate: accurate low-pass filter, close to real hardware -- warmer, more bass (default).
  - `3` -- oversampled: same as accurate but 2× over-sampled; the slowest mode, not recommended because of SDL re-sampling.

* `mt32_sampling_quality` -- quality of the libmt32emu emulator's internal resampler.
  - `0` -- fastest
  - `1` -- fast
  - `2` -- good (default)
  - `3` -- best

* `mt32_reverb` -- adds reverb to the MT-32 emulator's output signal (default: `true`).

These are optional; the defaults are fine for normal use.
