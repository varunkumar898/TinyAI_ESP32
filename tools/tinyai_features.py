"""Canonical tokenizer and hashed feature encoder for the ESP32 TinyAI model.

This module is the host-side reference implementation. The C implementation in
components/tinyml/tinyml.c must produce byte-for-byte identical tokens and
feature buckets; tools/export_model.py emits golden vectors that enforce this in
the native test suite.

Feature layout (all features are counts, not booleans):
  * unigram       "w:<token>"
  * bigram        "b:<token1> <token2>"
  * char 3..5gram "c3:^<token>$", "c4:...", "c5:..."

A 32-bit FNV-1a hash is reduced modulo FEATURE_BUCKETS. No signed hashing is
used. Collisions are learned during training and are deterministic on-device.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

MAX_TEXT_BYTES = 160
MAX_TOKENS = 24
MAX_TOKEN_CHARS = 24
FEATURE_BUCKETS = 2048
CHAR_NGRAM_MIN = 3
CHAR_NGRAM_MAX = 5

# The seven product intents. Pin/device configuration is deliberately NOT a
# model intent: an untrusted classifier must never reconfigure hardware.
# Configuration phrases are trained as UNKNOWN so the model rejects them, and
# the deterministic command layer additionally refuses configuration verbs.
INTENTS = [
    "TURN_ON",
    "TURN_OFF",
    "TOGGLE",
    "GET_STATUS",
    "LIST_DEVICES",
    "HELP",
    "UNKNOWN",
]

INTENT_INDEX = {name: idx for idx, name in enumerate(INTENTS)}

# ASCII only. Rejecting non-ASCII avoids homoglyph/Unicode confusables being
# silently interpreted as a different device name.
_ASCII_ALNUM = re.compile(r"[0-9a-zA-Z]")

# Target tokens are deliberately replaced before intent classification. Device
# names and GPIO numbers are not intent evidence, and leaving them in makes the
# tiny linear model memorize pin names instead of learning command verbs.
# Entity extraction still reads the original, unmasked tokens.
TARGET_TOKENS = {
    "led", "leds", "light", "lights", "lamp", "lamps", "bulb", "bulbs",
    "fan", "fans", "relay", "relays", "aux", "auxiliary", "pin", "pins",
    "gpio", "wire", "wires", "channel", "channels", "sensor", "sensors",
    "button", "buttons", "motion", "pir",
}
NUMBER_WORDS = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
}
MASK_TOKEN = "tg"


class TextError(ValueError):
    """Raised when text cannot be represented by the firmware tokenizer."""

    def __init__(self, status: str) -> None:
        super().__init__(status)
        self.status = status


@dataclass(frozen=True)
class Feature:
    bucket: int
    count: int


def _normalize(text: str) -> str:
    raw = text.encode("utf-8")
    if len(raw) > MAX_TEXT_BYTES:
        raise TextError("TOO_LONG")
    out: list[str] = []
    previous_space = True
    for ch in text:
        if ord(ch) >= 0x80:
            raise TextError("NON_ASCII")
        if ch.isascii() and _ASCII_ALNUM.fullmatch(ch):
            out.append(ch.lower())
            previous_space = False
        elif ch == "'":
            # Apostrophes are deleted: "don't" -> "dont". This keeps the
            # negation token recognisable and is trivial to mirror in C.
            continue
        else:
            if not previous_space:
                out.append(" ")
                previous_space = True
    return "".join(out).strip()


def tokenize(text: str) -> list[str]:
    norm = _normalize(text)
    if not norm:
        raise TextError("EMPTY")
    tokens = norm.split(" ")
    if len(tokens) > MAX_TOKENS:
        raise TextError("TOO_MANY_TOKENS")
    for token in tokens:
        if len(token) > MAX_TOKEN_CHARS:
            raise TextError("TOKEN_TOO_LONG")
    if not tokens:
        raise TextError("EMPTY")
    return tokens


def fnv1a32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def is_target_token(token: str) -> bool:
    if token in TARGET_TOKENS or token in NUMBER_WORDS:
        return True
    if token.isdigit():
        return True
    for prefix in ("led", "gpio", "pin"):
        if token.startswith(prefix) and token[len(prefix):].isdigit():
            return True
    return False


def mask_targets(tokens: list[str]) -> list[str]:
    return [MASK_TOKEN if is_target_token(token) else token for token in tokens]


def feature_keys(tokens: list[str]):
    for token in tokens:
        yield f"w:{token}"
    for left, right in zip(tokens, tokens[1:]):
        yield f"b:{left} {right}"
    for token in tokens:
        padded = f"^{token}$"
        for n in range(CHAR_NGRAM_MIN, CHAR_NGRAM_MAX + 1):
            if len(padded) < n:
                continue
            for index in range(0, len(padded) - n + 1):
                yield f"c{n}:{padded[index:index + n]}"


def feature_hash(key: str) -> int:
    return fnv1a32(key.encode("ascii")) % FEATURE_BUCKETS


def feature_counts(tokens: list[str]) -> dict[int, int]:
    counts: dict[int, int] = {}
    for key in feature_keys(tokens):
        bucket = feature_hash(key)
        counts[bucket] = counts.get(bucket, 0) + 1
    return counts


def sorted_features(tokens: list[str]) -> list[Feature]:
    return [Feature(b, c) for b, c in sorted(feature_counts(tokens).items())]
