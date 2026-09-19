"""Temporary: answers spelled in ASCII, because the device can draw nothing else.

The firmware shows a `?` for every character outside ASCII until it learns to
render Cyrillic (D1 in its docs/deferred.md). The model still answers in the
language it was asked in, and only the text that goes to the device is spelled
in Latin letters here. When the firmware gets there, delete this module and
its one call in main.py.
"""

from __future__ import annotations

_LETTERS = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "yo",
    "ж": "zh", "з": "z", "и": "i", "й": "y", "к": "k", "л": "l", "м": "m",
    "н": "n", "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u",
    "ф": "f", "х": "kh", "ц": "ts", "ч": "ch", "ш": "sh", "щ": "shch",
    "ъ": "", "ы": "y", "ь": "'", "э": "e", "ю": "yu", "я": "ya",
}  # fmt: skip

# Typography a Russian answer comes with, which would be `?` on the device too.
_PUNCTUATION = {
    "«": '"', "»": '"', "„": '"', "“": '"', "”": '"', "‘": "'", "’": "'",
    "—": "-", "–": "-", "…": "...", "№": "N", " ": " ", " ": " ",
}  # fmt: skip

_TABLE = str.maketrans(
    {
        **_LETTERS,
        **{upper.upper(): latin.capitalize() for upper, latin in _LETTERS.items()},
        **_PUNCTUATION,
    }
)


def transliterate(text: str) -> str:
    return text.translate(_TABLE)
