function _setCopyButtonText(buttonElement, label) {
    if (!buttonElement) {
        return;
    }

    buttonElement.textContent = label;
}

function _resetCopyButtonText(buttonElement) {
    if (!buttonElement) {
        return;
    }

    setTimeout(function () {
        _setCopyButtonText(buttonElement, 'Copy');
    }, 2000);
}

function _fallbackCopyText(textToCopy) {
    var tempTextArea = document.createElement('textarea');
    tempTextArea.value = textToCopy;
    tempTextArea.setAttribute('readonly', '');
    tempTextArea.style.position = 'fixed';
    tempTextArea.style.top = '-9999px';
    tempTextArea.style.left = '-9999px';
    document.body.appendChild(tempTextArea);
    tempTextArea.focus();
    tempTextArea.select();

    var copied = false;
    try {
        copied = document.execCommand('copy');
    } catch (error) {
        copied = false;
    }

    document.body.removeChild(tempTextArea);
    return copied;
}

function copyStorageContent(buttonElement) {
    var contentElement = document.getElementById('storage-content');
    if (!contentElement) {
        return false;
    }

    var textToCopy = contentElement.textContent || '';

    if (navigator && navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
        navigator.clipboard.writeText(textToCopy).then(function () {
            _setCopyButtonText(buttonElement, 'Copied!');
            _resetCopyButtonText(buttonElement);
        }).catch(function () {
            if (_fallbackCopyText(textToCopy)) {
                _setCopyButtonText(buttonElement, 'Copied!');
            } else {
                _setCopyButtonText(buttonElement, 'Copy failed');
            }
            _resetCopyButtonText(buttonElement);
        });

        return false;
    }

    if (_fallbackCopyText(textToCopy)) {
        _setCopyButtonText(buttonElement, 'Copied!');
    } else {
        _setCopyButtonText(buttonElement, 'Copy failed');
    }
    _resetCopyButtonText(buttonElement);

    return false;
}
