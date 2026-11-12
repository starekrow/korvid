#!/usr/bin/env python3
"""Arcfire's sounds and music: the vendored effects, copied unmodified under new names, and
the two music tracks, resampled. Stdlib only. Run from anywhere:

    python3 examples/arcfire/tools/make_sounds.py ~/code/asset-packs/arcfire

The argument is the directory the archives were downloaded and unpacked into (`pack/`
under it holds one directory per archive). Writes examples/arcfire/assets/sound/
(Kenney's Sci-Fi Sounds and Impact Sounds, CC0, as they are) and assets/music/ (Juhani
Junkala's 5 Chiptunes (Action), CC0: four tracks, Shoreline's and Orbit's, folded to mono and halved in rate, 22,050
Hz 16-bit WAV, about a quarter of the size; the engine streams only Ogg Vorbis, and
nothing here encodes it).
assets/CREDITS.md lists every file.
"""
import array
import shutil
import sys
import wave
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"
SCIFI = "pack/kenney_sci-fi-sounds/Audio"
IMPACT = "pack/kenney_impact-sounds/Audio"
MUSIC = "pack/juhani_5_action_chiptunes"

# (here, the pack, in the pack): what each sound is for in the game.
EFFECTS = [
    ("shot.ogg", SCIFI, "laserSmall_000.ogg"),               # the ship's volley
    ("hit.ogg", IMPACT, "impactPlate_light_000.ogg"),        # a shot on an enemy
    ("ping.ogg", IMPACT, "impactMetal_light_001.ogg"),       # a shot on armor
    ("kill.ogg", SCIFI, "explosionCrunch_000.ogg"),          # popcorn shot down
    ("kill_big.ogg", SCIFI, "explosionCrunch_002.ogg"),      # a ground row or a boss's part
    ("boss_down.ogg", SCIFI, "explosionCrunch_004.ogg"),     # a boss's core
    ("rumble.ogg", SCIFI, "lowFrequency_explosion_000.ogg"), # the bomb's blast, a hull breaking
    ("graze.ogg", SCIFI, "laserRetro_000.ogg"),              # a bullet through the ring
    ("chip.ogg", SCIFI, "laserRetro_002.ogg"),               # an item picked up
    ("bomb.ogg", SCIFI, "forceField_000.ogg"),               # the bomb's flash
    ("egg_break.ogg", SCIFI, "lowFrequency_explosion_001.ogg"), # the ship breaks
    ("egg_open.ogg", IMPACT, "impactMetal_heavy_000.ogg"),   # the egg falls away in halves
    ("egg_bloom.ogg", SCIFI, "forceField_002.ogg"),          # the new ship, the shell
    ("warning.ogg", SCIFI, "computerNoise_000.ogg"),         # a boss comes; a boss stomps
    ("ui_move.ogg", SCIFI, "laserRetro_001.ogg"),            # a menu's focus moves
    ("ui_select.ogg", SCIFI, "forceField_001.ogg"),          # a menu item chosen
    ("ui_cancel.ogg", SCIFI, "doorClose_001.ogg"),           # back out of a menu
]

TRACKS = [
    ("stage.wav", "Juhani Junkala [Retro Game Music Pack] Level 1.wav"),
    ("boss.wav", "Juhani Junkala [Retro Game Music Pack] Level 3.wav"),
    ("orbit.wav", "Juhani Junkala [Retro Game Music Pack] Level 2.wav"),
    ("orbit_boss.wav", "Juhani Junkala [Retro Game Music Pack] Ending.wav"),
]


def halve_to_mono(src, dst):
    """16-bit stereo to 16-bit mono at half the rate: each output sample the mean of two
    frames' four samples (a box filter before the decimation)."""
    with wave.open(str(src), "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 2, src
        rate = w.getframerate()
        pcm = array.array("h", w.readframes(w.getnframes()))
    n = len(pcm) // 4 * 4
    out = array.array("h", ((a + b + c + d) // 4 for a, b, c, d in zip(pcm[0:n:4], pcm[1:n:4], pcm[2:n:4], pcm[3:n:4])))
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate // 2)
        w.writeframes(out.tobytes())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pack = Path(sys.argv[1]).expanduser()
    (ASSETS / "sound").mkdir(parents=True, exist_ok=True)
    (ASSETS / "music").mkdir(parents=True, exist_ok=True)
    for here, where, there in EFFECTS:
        shutil.copyfile(pack / where / there, ASSETS / "sound" / here)
    shutil.copyfile(pack / SCIFI / ".." / "License.txt", ASSETS / "LICENSE-Kenney-SciFiSounds.txt")
    shutil.copyfile(pack / IMPACT / ".." / "License.txt", ASSETS / "LICENSE-Kenney-ImpactSounds.txt")
    for here, there in TRACKS:
        halve_to_mono(pack / MUSIC / there, ASSETS / "music" / here)
    shutil.copyfile(pack / MUSIC / "INFO.txt", ASSETS / "LICENSE-JuhaniJunkala.txt")
    print(f"{len(EFFECTS)} effects, {len(TRACKS)} tracks")


if __name__ == "__main__":
    main()
