"""Check the Pages OTA notes against the device's manifest constraints."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from add_xuesu_release_notes import extract_release_notes


class XuesuReleaseNotesTest(unittest.TestCase):
    def test_extracts_release_text_from_markdown(self):
        body = '''## 更新内容

- **上传字体**。支持 `.ttf` 和 `.cpfont`。
1. USB &amp; SD 卡。请按[设备提示](https://example.com/help)操作。
<!-- Hidden release metadata -->

```text
Ignored example
```

[打开雪酥刷机页](https://example.com/)
'''
        self.assertEqual(extract_release_notes(body), [
            '上传字体。支持 .ttf 和 .cpfont。',
            'USB & SD 卡。请按设备提示操作。',
        ])

    def test_wraps_multibyte_text_and_bounds_the_number_of_notes(self):
        sentence = '甲' * 25 + '。'
        rest = '雪' * 30 + '📖甲乙'
        body = f'- {sentence}{rest}\n' + '\n'.join(f'- 更新 {number}' for number in range(10))
        notes = extract_release_notes(body)
        self.assertEqual(notes[:3], [sentence, '雪' * 30 + '📖', '甲乙'])
        self.assertEqual(notes[3:], [f'更新 {number}' for number in range(5)])
        self.assertTrue(all(len(note.encode('utf-8')) <= 96 for note in notes))

    def test_cli_refreshes_notes_and_preserves_firmware_metadata(self):
        manifest = {
            'schema': 1,
            'tag_name': '0.1.9',
            'release_tag': 'xuesu-v0.1.9',
            'assets': [{'name': 'firmware.bin', 'size': 1024, 'digest': 'sha256:' + 'a' * 64}],
        }
        with tempfile.TemporaryDirectory() as directory:
            manifest_path = Path(directory) / 'latest.json'
            body_path = Path(directory) / 'release.md'
            manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
            command = [sys.executable, str(SCRIPTS / 'add_xuesu_release_notes.py'), str(manifest_path), str(body_path)]

            body_path.write_bytes('- 上传字体\r\n- USB 磁盘模式\r\n'.encode('utf-8'))
            subprocess.run(command, check=True)
            self.assertEqual(json.loads(manifest_path.read_text(encoding='utf-8')), {
                **manifest, 'release_notes': ['上传字体', 'USB 磁盘模式'],
            })

            body_path.write_text('- 只有一条\n- **只有一条**\n', encoding='utf-8')
            subprocess.run(command, check=True)
            self.assertEqual(json.loads(manifest_path.read_text(encoding='utf-8')), manifest)
