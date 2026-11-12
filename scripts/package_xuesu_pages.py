#!/usr/bin/env python3
"""Stage the verified Xuesu OTA1 app and the Pages update manifest."""

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re

from package_nightly_target import verify_firmware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('.pio/build/xuesu_300'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--published-at', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--repository', required=True)
    args = parser.parse_args()

    if not re.fullmatch(r'xuesu-v[0-9]+\.[0-9]+\.[0-9]+', args.version):
        parser.error('Expected a release tag such as xuesu-v0.1.0')
    repository = re.fullmatch(r'([A-Za-z0-9-]+)/([A-Za-z0-9_.-]+)', args.repository)
    if not re.fullmatch(r'[0-9a-f]{40}', args.commit) or repository is None:
        parser.error('Expected a full Git commit SHA and an owner/repository name')
    if datetime.fromisoformat(args.published_at.replace('Z', '+00:00')).utcoffset() is None:
        parser.error('The publish time must include a timezone')

    partitions = (args.build / 'partitions.bin').read_bytes()
    if hashlib.sha256(partitions).hexdigest() != '6bc034d19c972844b71b0bc45ef77401b018cc2ec08cc7440193c87fd11149ab':
        raise SystemExit('The build partition table differs from the verified Xuesu layout')
    app = args.build / 'firmware.bin'
    verify_firmware(app, chip_id=9, board_tag='xuesu_300')
    data = app.read_bytes()
    if not 64 <= len(data) <= 0x730000 or len(data) % 16:
        raise SystemExit('The application image does not fit the Xuesu OTA1 slot')
    if data[23] != 1 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise SystemExit('The ESP32-S3 image digest is invalid')

    digest = hashlib.sha256(data).hexdigest()
    firmware = args.output / 'firmware'
    firmware.mkdir(parents=True, exist_ok=True)
    (firmware / f'{digest}.bin').write_bytes(data)

    owner, repo = repository.groups()
    host = f'{owner.lower()}.github.io'
    site_url = f'https://{host}' if repo.lower() == host else f'https://{host}/{repo}'
    metadata = {
        'schema': 1,
        'board': 'xuesu-300-std',
        'target_partition': 'ota_1',
        'tag_name': args.version.removeprefix('xuesu-v'),
        'release_tag': args.version,
        'published_at': args.published_at,
        'commit': args.commit,
        'repository': args.repository,
        'assets': [{
            'name': 'firmware.bin',
            'size': len(data),
            'browser_download_url': f'{site_url}/firmware/{digest}.bin',
            'digest': f'sha256:{digest}',
            'md5': hashlib.md5(data, usedforsecurity=False).hexdigest(),
        }],
    }
    manifest = args.output / 'api/v1/firmware/latest.json'
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
