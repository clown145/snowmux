    function fontUploadNames(name) {
      const dot = name.lastIndexOf('.');
      const ext = name.slice(dot).toLowerCase();
      const stem = name.slice(0, dot).normalize('NFC').trim()
        .replace(/[^\p{L}\p{M}\p{N}_-]+/gu, '_').replace(/^[_-]+|[_-]+$/g, '');
      if (!stem) return null;
      if (ext === '.cpfont') {
        const size = stem.match(/[_-](\d{1,3})$/);
        if (!size || Number(size[1]) < 1 || Number(size[1]) > 255) return null;
        const family = stem.slice(0, -size[0].length);
        return {family, filename: family + '_' + size[1] + ext};
      }
      const style = /[_-](?:(?:extra[_-]?|ultra[_-]?|semi[_-]?|demi[_-]?)?(?:light|bold|black)|thin|medium|regular|normal|book|heavy)(?:[_-]?(?:italic|oblique))?$|[_-](?:italic|oblique)$/i;
      return {family: stem.replace(style, ''), filename: stem + ext};
    }
