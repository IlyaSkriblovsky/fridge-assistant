#!/usr/bin/env python3
"""E16 computer listening test. Standard library only; macOS uses afplay.

Run: python3 firmware/tools/e16_melodies.py
Export without playback: python3 firmware/tools/e16_melodies.py --export-wav /tmp/e16
"""

import argparse
from array import array
from datetime import datetime, timezone
import itertools
import json
import math
from pathlib import Path
import random
import secrets
import shutil
import subprocess
import sys
import tempfile
import wave


PATTERNS = {
    "repeated-call": [0, 2, 0, 2, 0, 2, 0, 2],
    "rising-answer": [0, 2, 0, 2, 1, 3, 1, 3],
    "wave": [0, 2, 4, 2, 0, -2, 0, 2],
    "turn-and-answer": [0, -1, 2, 1, 0, 2, 1, 3],
    "stepped-fanfare": [0, 0, 2, 0, 2, 2, 4, 2],
    "return-home": [0, 2, 4, 2, 3, 1, 2, 0],
}
RATE = 48000
NOTE_SAMPLES = 1920  # 40 ms
REST_SAMPLES = 2400  # 50 ms
PAIR_GAP_SAMPLES = 43200  # 900 ms


def synthesize(offsets, volume):
    """Band-limited 50% square wave; preserve phase through pitch changes.

    Odd harmonics below Nyquist approximate a buzzer drive without aliasing
    a naive sampled square wave. This does not model the physical transducer.
    A fixed harmonic sum divisor preserves headroom without per-phrase AGC.
    """
    amplitude = round(32767 * volume)
    samples = array("h")
    for element, offset in enumerate(offsets):
        phase = 0.0
        factor = 2 ** (offset / 12)
        for note in range(8):
            hz = math.floor((3550 if note % 2 == 0 else 4150) * factor + 0.5)
            harmonics = list(range(1, math.ceil((RATE / 2) / hz), 2))
            for _ in range(NOTE_SAMPLES):
                value = sum(math.sin(h * phase) / h for h in harmonics) / 2
                samples.append(round(amplitude * value))
                phase = (phase + 2 * math.pi * hz / RATE) % (2 * math.pi)
        if element + 1 < len(offsets):
            samples.extend([0] * REST_SAMPLES)
    return samples


def write_wav(path, samples):
    pcm = array("h", samples)
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm.tobytes())


def player():
    if sys.platform == "win32":
        import winsound
        return lambda path: winsound.PlaySound(str(path), winsound.SND_FILENAME)
    for executable, flags in [
        ("afplay", []),
        ("aplay", ["-q"]),
        ("ffplay", ["-nodisp", "-autoexit", "-loglevel", "error"]),
    ]:
        resolved = shutil.which(executable)
        if resolved:
            return lambda path: subprocess.run(
                [resolved, *flags, str(path)], check=True
            )
    raise RuntimeError("Не найден проигрыватель: нужен afplay, aplay или ffplay.")


def save(path, result):
    """Replace only this run's log; caller exclusively reserves the path."""
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, delete=False
    ) as temporary:
        json.dump(result, temporary, ensure_ascii=False, indent=2)
        temporary.write("\n")
        name = temporary.name
    Path(name).replace(path)


def run_test(result, rng, present, ask, persist):
    """Interactive ranking; playback failure can never become a vote."""
    def trial(stage, a, b):
        record = {
            "trial": len(result["trials"]) + 1, "stage": stage,
            "a": a, "b": b, "presentations": 0, "choice": None,
        }
        result["trials"].append(record)
        persist()
        while True:
            print(f"\nПара {record['trial']}/20: сначала A, затем B.")
            present(a, b)
            record["presentations"] += 1
            persist()
            while True:
                answer = ask("A/B — выбор, R или Enter — повтор, Q — выйти: ").strip().lower()
                if answer in ("a", "b", "r", "", "q"):
                    break
                print("Введи A, B, R или Q.")
            if answer == "q":
                raise KeyboardInterrupt
            if answer in ("a", "b"):
                record["choice"] = answer
                record["winner"] = a if answer == "a" else b
                persist()
                return record["winner"]

    pairs = list(itertools.combinations(PATTERNS, 2))
    rng.shuffle(pairs)
    scores = dict.fromkeys(PATTERNS, 0)
    for a, b in pairs:
        if rng.randrange(2):
            a, b = b, a
        scores[trial("round-robin", a, b)] += 1
    result["scores"] = scores
    rank = list(PATTERNS)
    rng.shuffle(rank)
    rank.sort(key=lambda name: scores[name], reverse=True)
    result["cutoff_tie"] = scores[rank[1]] == scores[rank[2]]
    finalists = rank[:2]
    result["finalists"] = finalists
    final_scores = dict.fromkeys(finalists, 0)
    result["final_scores"] = final_scores
    reverse = rng.randrange(2)
    print("\nОсновной этап завершён. Ещё пять сравнений двух лидеров.")
    for index in range(5):
        a, b = finalists
        if (index + reverse) % 2:
            a, b = b, a
        final_scores[trial("final", a, b)] += 1
        persist()
    result["winner"] = max(final_scores, key=final_scores.get)
    result["status"] = "complete"
    persist()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, help="Seed for reproducible pair order")
    parser.add_argument("--volume", type=float, default=0.2,
                        help="Digital level, 0 < value <= 1 (default: 0.2)")
    parser.add_argument("--output", type=Path, help="New JSON result file")
    parser.add_argument("--export-wav", type=Path,
                        help="Export six WAV files and exit without playback")
    args = parser.parse_args(argv)
    if not math.isfinite(args.volume) or not 0 < args.volume <= 1:
        parser.error("--volume must be greater than 0 and at most 1")
    if args.export_wav and args.output:
        parser.error("--output and --export-wav are separate modes")

    audio = {name: synthesize(offsets, args.volume) for name, offsets in PATTERNS.items()}
    if args.export_wav:
        args.export_wav.mkdir(parents=True, exist_ok=True)
        paths = {name: args.export_wav / f"{name}.wav" for name in PATTERNS}
        if any(path.exists() for path in paths.values()):
            parser.error("WAV files already exist in the export directory")
        for name, path in paths.items():
            write_wav(path, audio[name])
        print(f"Шесть WAV по 2,91 с сохранены в {args.export_wav.resolve()}")
        return 0

    play = player()
    seed = args.seed if args.seed is not None else secrets.randbits(32)
    now = datetime.now(timezone.utc)
    output = args.output or Path(f"e16-computer-{now:%Y%m%d-%H%M%S-%f}.json")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite a prior experiment.
    with output.open("x", encoding="utf-8"):
        pass
    result = {
        "experiment": "E16-computer", "status": "running",
        "started_utc": now.isoformat(), "seed": seed,
        "platform": sys.platform, "python": sys.version,
        "patterns": PATTERNS, "base_hz": [3550, 4150],
        "sample_rate": RATE, "synthesis": "band-limited-square-v1",
        "volume": args.volume, "note_ms": 40, "notes_per_element": 8,
        "element_gap_ms": 50, "pair_gap_ms": 900, "trials": [],
    }
    persist = lambda: save(output, result)
    persist()
    print("E16: шесть мелодий по восемь полных трелей, 2,91 с каждая.")
    print("Выбирай приятный и заметный сигнал для важного уведомления.")
    print("В каждой паре: A целиком, пауза 0,9 с, затем B целиком.")
    print("Названия раскроются после завершения. Громкость системы держи постоянной.")
    print(f"Результаты сохраняются после каждого действия: {output}")
    try:
        with tempfile.TemporaryDirectory(prefix="e16-audio-") as directory:
            pair_path = Path(directory) / "pair.wav"

            def present(a, b):
                pcm = audio[a] + array("h", [0]) * PAIR_GAP_SAMPLES + audio[b]
                write_wav(pair_path, pcm)
                play(pair_path)

            if input("Enter — начать, Q — выйти: ").strip().lower() == "q":
                raise KeyboardInterrupt
            run_test(result, random.Random(seed), present, input, persist)
    except (KeyboardInterrupt, EOFError):
        result["status"] = "interrupted"
        persist()
        print("\nТест остановлен; все сделанные выборы сохранены.")
        return 0
    except Exception as error:
        result["status"] = "error"
        result["error"] = str(error)
        persist()
        raise
    print("\nРезультаты основного этапа:")
    for name in sorted(result["scores"], key=result["scores"].get, reverse=True):
        print(f"  {name}: {result['scores'][name]}/5 — {PATTERNS[name]}")
    if result["cutoff_tie"]:
        print("При равенстве баллов участник финала выбран случайно; учтём это при оценке.")
    print(f"Финал: {result['final_scores']}")
    print(f"Победитель: {result['winner']} — {PATTERNS[result['winner']]}")
    print(f"Журнал: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Ошибка: {error}", file=sys.stderr)
        raise SystemExit(1)
