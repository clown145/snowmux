    function uploadFontFile({file, names}, onProgress, onSent) {
      return new Promise(resolve => {
        const formData = new FormData();
        formData.append('family', names.family);
        formData.append('file', file, names.filename);

        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/fonts/upload');
        xhr.responseType = 'json';
        xhr.upload.onprogress = event => {
          if (event.lengthComputable && event.total > 0) onProgress(event.loaded / event.total);
        };
        xhr.upload.onload = () => {
          onProgress(1);
          onSent();
        };
        xhr.onload = () => resolve(xhr.status === 200 && xhr.response?.ok === true);
        xhr.onerror = xhr.onabort = () => resolve(false);
        xhr.send(formData);
      });
    }

    function setFontUploadPercent(value) {
      const percent = Math.floor(value);
      fontUploadBar.value = percent;
      fontUploadPercent.textContent = percent + '%';
    }
