#!/usr/bin/env python3
"""Add device-sized notes from a GitHub Release body to the Xuesu OTA manifest."""

import argparse
import html
import json
from pathlib import Path
import re

from generate_ota_notes import FORBIDDEN, MAX_NOTE_BYTES, MAX_NOTES, MIN_NOTES


FENCED_CODE = re.compile(r'^[ \t]*(`{3,}|~{3,})[^\n]*\n.*?(?:^[ \t]*\1[ \t]*$|\Z)', re.MULTILINE | re.DOTALL)
LINK = re.compile(r'!?\[([^\]]*)\]\([^)]*\)')
HEADING = re.compile(r'^#{1,6}(?:\s|$)')
LIST_PREFIX = re.compile(r'^(?:[-+*]|\d+[.)])\s+')
SEPARATOR = re.compile(r'-{3,}|\*{3,}|_{3,}')
REMOVE_MARKUP = str.maketrans('', '', ''.join(FORBIDDEN))


def wrap_note(text: str):
    while len(encoded := text.encode('utf-8')) > MAX_NOTE_BYTES:
        # Keep UTF-8 characters intact and prefer a nearby punctuation or word boundary.
        prefix = encoded[:MAX_NOTE_BYTES].decode('utf-8', errors='ignore')
        for break_chars in ('。！？；', '，、', ' '):
            end = max(prefix.rfind(char) + 1 for char in break_chars)
            if end >= len(prefix) // 2:
                break
        else:
            end = len(prefix)
        yield text[:end].rstrip()
        text = text[end:].lstrip()
    if text:
        yield text


def extract_release_notes(body: str) -> list[str]:
    body = FENCED_CODE.sub('', re.sub(r'<!--.*?(?:-->|\Z)', '', body, flags=re.DOTALL))
    notes = []
    for line in body.splitlines():
        line = line.strip()
        if HEADING.match(line) or SEPARATOR.fullmatch(line):
            continue
        line = LIST_PREFIX.sub('', line)
        if not line or LINK.fullmatch(line):
            continue
        text = html.unescape(LINK.sub(r'\1', line)).translate(REMOVE_MARKUP)
        text = ' '.join(re.sub(r'[\x00-\x1f\x7f]', ' ', text).split())
        for note in wrap_note(text):
            if note not in notes:
                notes.append(note)
                if len(notes) == MAX_NOTES:
                    return notes
    return notes if len(notes) >= MIN_NOTES else []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('release_body', type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    notes = extract_release_notes(args.release_body.read_text(encoding='utf-8'))
    if notes:
        manifest['release_notes'] = notes
    else:
        manifest.pop('release_notes', None)
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
