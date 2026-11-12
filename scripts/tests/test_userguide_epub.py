#!/usr/bin/env python3
"""Validate the actual bundled archives, including links and reproducible bytes."""
import io
from pathlib import Path
import posixpath
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import generate_userguide_epub as generator


class UserGuideEpubTest(unittest.TestCase):
    def test_bundled_epubs(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            header = output / 'guide.generated.h'
            generator.build_bundled(ROOT / 'docs/user-guide', output, header)
            before = header.read_bytes()
            sizes = {'CrossMux': 0, 'SnowMux': 0}
            for creator, language, title, chapter_count in (
                ('CrossMux', 'zh-CN', 'CrossMux用户手册', 11),
                ('CrossMux', 'en', 'CrossMux User Guide', 11),
                ('SnowMux', 'zh-CN', 'SnowMux 使用说明书', 8),
                ('SnowMux', 'en', 'SnowMux User Guide', 8),
            ):
                data = (output / f'{title}.epub').read_bytes()
                sizes[creator] += len(data)
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    self.assertIsNone(archive.testzip())
                    first = archive.infolist()[0]
                    self.assertEqual(first.filename, 'mimetype')
                    self.assertEqual(first.compress_type, zipfile.ZIP_STORED)
                    self.assertEqual(archive.read(first), b'application/epub+zip')
                    ns = {'o': 'http://www.idpf.org/2007/opf',
                          'dc': 'http://purl.org/dc/elements/1.1/',
                          'c': 'urn:oasis:names:tc:opendocument:xmlns:container',
                          'x': 'http://www.w3.org/1999/xhtml'}
                    container = ET.fromstring(archive.read('META-INF/container.xml'))
                    opf_path = container.find('c:rootfiles/c:rootfile', ns).get('full-path')
                    opf = ET.fromstring(archive.read(opf_path))
                    self.assertEqual(opf.find('o:metadata/dc:title', ns).text, title)
                    self.assertEqual(opf.find('o:metadata/dc:creator', ns).text, creator)
                    self.assertEqual(opf.find('o:metadata/dc:language', ns).text, language)
                    items = {item.get('id'): item for item in opf.findall('o:manifest/o:item', ns)}
                    for item in items.values():
                        self.assertRegex(item.get('id'), r'^[A-Za-z_][\w.-]*$')
                        self.assertIn('OEBPS/' + item.get('href'), archive.namelist())
                        self.assertNotIn('font', item.get('media-type'))
                    spine = [item.get('idref') for item in opf.findall('o:spine/o:itemref', ns)]
                    self.assertEqual(len(spine), chapter_count)
                    self.assertNotIn('nav', spine)
                    self.assertNotIn('style', spine)
                    self.assertTrue(all(item in items for item in spine))
                    self.assertEqual(items['style'].get('media-type'), 'text/css')
                    self.assertEqual(archive.read('OEBPS/style.css'),
                                     (ROOT / 'docs/user-guide/style.css').read_bytes())
                    self.assertIsNone(opf.find('o:metadata/o:meta[@name="cover"]', ns))
                    self.assertNotIn('OEBPS/cover.png', archive.namelist())
                    self.assertNotIn('OEBPS/cover.xhtml', archive.namelist())
                    self.assertIn('OEBPS/' + spine[0] + '.xhtml', archive.namelist())
                    if creator == 'CrossMux':
                        section = ET.fromstring(archive.read('OEBPS/' + spine[5] + '.xhtml'))
                        self.assertEqual(len(section.findall('.//x:h3', ns)), 2)
                        self.assertTrue(section.findall('.//x:blockquote', ns))
                        fonts = ET.fromstring(archive.read('OEBPS/' + spine[6] + '.xhtml'))
                        font_text = ''.join(fonts.itertext())
                        self.assertIn('字体文件已损坏' if language == 'zh-CN' else 'Damaged font file', font_text)
                        self.assertIn('.cpfont', font_text)
                        troubleshooting = ET.fromstring(archive.read('OEBPS/' + spine[9] + '.xhtml'))
                        self.assertEqual(len(troubleshooting.findall('.//x:h3', ns)), 4)
                        self.assertTrue(any((p.text or '').startswith('1. ') for p in
                                            troubleshooting.findall('.//x:p', ns)))
                    for chapter in spine:
                        section = ET.fromstring(archive.read('OEBPS/' + items[chapter].get('href')))
                        self.assertTrue(section.findall('.//x:link[@rel="stylesheet"]', ns))
                    for name in archive.namelist():
                        if name.endswith(('.xml', '.opf', '.xhtml')):
                            document = ET.fromstring(archive.read(name))
                            if name.endswith('.xhtml'):
                                self.assertEqual(document.get('{http://www.w3.org/XML/1998/namespace}lang'), language)
                            for element in document.iter():
                                for attr in ('href', 'src'):
                                    link = element.get(attr)
                                    if link and not link.startswith(('http:', 'https:', '#')):
                                        target = posixpath.normpath(posixpath.join(posixpath.dirname(name), link))
                                        self.assertIn(target.split('#')[0], archive.namelist())
                    nav = ET.fromstring(archive.read('OEBPS/nav.xhtml'))
                    self.assertEqual([link.get('href') for link in nav.findall('.//x:nav/x:ol/x:li/x:a', ns)],
                                     [items[chapter].get('href') for chapter in spine])
            for total in sizes.values():
                self.assertLessEqual(total, generator.BUNDLED_BUDGET)
            generator.build_bundled(ROOT / 'docs/user-guide', output, header)
            self.assertEqual(before, header.read_bytes())


if __name__ == '__main__':
    unittest.main()
