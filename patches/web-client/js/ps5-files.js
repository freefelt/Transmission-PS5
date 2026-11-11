/* The console browser has no working file chooser (the stock <input type="file"> shows an error), so the "Add Torrents"
 * dialog gets a button that opens the shared picker (ps5-picker.js) in the file mode: the disks of the console, then the
 * *.torrent files on them. The chosen file is added by its path: the daemon reads the .torrent itself (the stock
 * torrent-add with "filename" does that), so nothing is uploaded through the page.
 *
 * Only the console gets the button; on a computer the native file chooser stays. The list comes from the daemon's
 * read-only RPC method ps5_files (the manager is not involved).
 *
 * License: GPL-3.0-or-later (like Transmission).
 */
(function () {
    'use strict';
    if (!/PlayStation/i.test(navigator.userAgent || '')) {
        return;
    }

    var sid = '';
    function rpc(method, args, retried) {
        var headers = { 'Content-Type': 'application/json' };
        if (sid) headers['X-Transmission-Session-Id'] = sid;
        return fetch('../rpc', {
            method: 'POST',
            headers: headers,
            body: JSON.stringify({ jsonrpc: '2.0', method: method, params: args || {}, id: 'webui' })
        }).then(function (r) {
            if (r.status === 409 && !retried) {
                sid = r.headers.get('X-Transmission-Session-Id') || '';
                return rpc(method, args, true);
            }
            if (!r.ok) throw new Error('http ' + r.status);
            return r.json();
        });
    }

    var ERRORS = {
        outside: 'This folder is outside the allowed places.',
        nodir: 'The folder does not exist or cannot be read.',
        badpath: 'Invalid path.'
    };
    function errorText(code) {
        return ps5t(ERRORS[code] || 'Could not open the folder');
    }

    function list(path) {
        return rpc('ps5_files', { path: path }).then(function (j) {
            var r = j && j.result;
            if (!r || r.error) throw new Error(errorText(r && r.error));
            return r;
        });
    }
    function disks() {
        return rpc('ps5_files', {}).then(function (j) {
            var r = j && j.result;
            return (r && r.disks) || [];
        });
    }

    var chosen = null;

    function showError(name, text) {
        var overlay = document.createElement('div');
        overlay.className = 'ps5-modal ps5-red';   // an error: red
        var box = document.createElement('div');
        box.className = 'ps5-modal-box';
        var h = document.createElement('h3');
        h.textContent = ps5t('Error adding "{1}"', [name]);
        var p = document.createElement('p');
        p.textContent = text || '';
        var bar = document.createElement('div');
        bar.className = 'ps5-modal-buttons';
        var ok = document.createElement('button');
        ok.type = 'button';
        ok.textContent = ps5t('Close');
        ok.addEventListener('click', function () { overlay.remove(); });
        bar.appendChild(ok);
        box.appendChild(h);
        box.appendChild(p);
        box.appendChild(bar);
        overlay.appendChild(box);
        document.body.appendChild(overlay);
        ok.focus();
    }

    function chosenName() {
        return chosen ? chosen.name : '';
    }

    function addChosen(root) {
        if (!chosen) return;
        var folder = root.querySelector('#add-dialog-folder-input');
        var start = root.querySelector('#auto-start-check');
        var name = chosenName();
        rpc('torrent_add', {
            filename: chosen.path,
            download_dir: folder ? folder.value : '',
            paused: !(start && start.checked)
        }).then(function (j) {
            if (j && j.error) {
                var data = j.error.data || {};
                showError(name, data.errorString || data.error_string || j.error.message);
            }
        }, function (e) {
            showError(name, (e && e.message) || ps5t('Could not open the folder'));
        });
    }

    function build(root) {
        if (!root || !root.querySelector || root.getAttribute('data-ps5-file')) return;
        var fileInput = root.querySelector('input[type="file"]');
        if (!fileInput) return;
        root.setAttribute('data-ps5-file', '1');
        chosen = null;

        var row = document.createElement('div');
        row.id = 'ps5-file-row';
        var button = document.createElement('button');
        button.type = 'button';
        button.id = 'ps5-file-btn';
        button.textContent = ps5t('Choose a .torrent file');
        var hint = document.createElement('div');
        hint.className = 'ps5-file-hint';
        hint.textContent = ps5t('The file is on a disk of the console (Data, M.2 SSD or a USB drive).');
        row.appendChild(button);
        row.appendChild(hint);
        fileInput.parentNode.insertBefore(row, fileInput);

        var chip = document.createElement('div');
        chip.id = 'ps5-file-chip';
        chip.style.display = 'none';
        var chipName = document.createElement('span');
        chipName.className = 'ps5-file-chip-name';
        var remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'ps5-file-chip-x';
        remove.textContent = '×';
        remove.setAttribute('aria-label', ps5t('Remove the chosen file'));
        chip.appendChild(chipName);
        chip.appendChild(remove);
        row.parentNode.insertBefore(chip, row.nextSibling);

        function renderChip() {
            if (chosen) {
                chip.style.display = 'flex';
                chipName.textContent = chosen.path;
            } else {
                chip.style.display = 'none';
                chipName.textContent = '';
            }
        }
        renderChip();

        function openPicker() {
            ps5Picker.open({
                overlayId: 'ps5-file-picker',
                title: ps5t('Choose a .torrent file'),
                mode: 'file',
                extensions: ['.torrent'],
                chooseLabel: ps5t('Choose this file'),
                disks: disks,
                list: list,
                onChoose: function (entry) {
                    chosen = entry;
                    renderChip();
                }
            });
        }

        button.addEventListener('click', openPicker);
        remove.addEventListener('click', function () {
            chosen = null;
            renderChip();
        });

        var confirm = root.querySelector('.dialog-buttons button:last-child');
        root.addEventListener('click', function (ev) {
            if (chosen && confirm && ev.target === confirm) addChosen(root);
        }, true);
    }

    new MutationObserver(function (mutations) {
        for (var i = 0; i < mutations.length; i++) {
            for (var k = 0; k < mutations[i].addedNodes.length; k++) {
                var n = mutations[i].addedNodes[k];
                if (n.nodeType === 1 && n.querySelector && n.querySelector('#add-dialog-folder-input')) build(n);
            }
        }
    }).observe(document.documentElement, { childList: true, subtree: true });
})();
