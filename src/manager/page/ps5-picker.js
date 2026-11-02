/* The shared picker of the PS5 settings page (port 9092, download folders) and of the Transmission web interface
 * (port 9091, .torrent files). The caller passes the data source and the rules:
 *
 *   ps5Picker.open({
 *     overlayId: 'fdPickModal',       // the id of the overlay (the caller's CSS/tests may refer to it)
 *     overlayClass: 'modal',          // the page's modal classes (the web client uses ps5-modal)
 *     boxClass: 'modal-card pk-card',
 *     title: 'Choose a download folder',
 *     mode: 'folder' | 'file',        // folders only, or folders for navigation + files to choose
 *     extensions: ['.torrent'],       // file mode: what may be chosen
 *     chooseLabel: 'Choose this folder',
 *     disks: function () { return [...] or Promise },     // [{root, kind, fs, fsbad, total, free}]
 *     list: function (path) { return Promise },           // {path, root, dirs, files, truncated}
 *     mkdir: function (path, name) { return Promise },    // optional: shows "New folder"
 *     canChoose: function (entry) { return bool },        // optional extra rule (e.g. not /data itself)
 *     cannotChooseText: '...',                            // shown when canChoose is false
 *     diskBad: function (disk) { return bool },           // optional "not recommended" note
 *     onChoose: function (entry) { ... }                  // {path, isDir, name, size}
 *   });
 *
 * The module knows nothing about HTTP: the pages pass their own list/disks functions (the manager uses /api/fs,
 * the web interface uses the daemon RPC method ps5_files). The texts are ps5t() keys; on the settings page the stub
 * below returns the text that lang_localize has already translated in this file.
 *
 * License: GPL-3.0-or-later (like Transmission).
 */
(function () {
    'use strict';

    if (typeof window.ps5t !== 'function') {
        window.ps5t = function (s, a) {
            if (!a) return s;
            return String(s).replace(/\{(\d)\}/g, function (m, i) {
                var v = a[i - 1];
                return v === undefined ? m : String(v);
            });
        };
    }

    var FOLDER_ICO = '<svg class="pk-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6.5A2.5 2.5 0 0 1 5.5 4h4l2 2.2h7A2.5 2.5 0 0 1 21 8.7v9.8a2.5 2.5 0 0 1-2.5 2.5h-13A2.5 2.5 0 0 1 3 18.5z"/></svg>';
    var FILE_ICO = '<svg class="pk-ico pk-file" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 3h7l5 5v11.5A1.5 1.5 0 0 1 17.5 21h-10A1.5 1.5 0 0 1 6 19.5v-15A1.5 1.5 0 0 1 7.5 3zm6.2 1.8V9h4.2z"/></svg>';
    var UP_ICO = '<svg class="pk-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5l-7 7h4v7h6v-7h4z"/></svg>';

    var KIND = { ssd: ps5t('M.2 SSD'), usb: ps5t('USB drive'), int: ps5t('Internal drive') };

    function size(n) {
        var u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0;
        while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
        return ps5t('{1} ' + u[i], [n >= 100 || i === 0 ? Math.round(n) : Math.round(n * 10) / 10]);
    }

    var state = {
        opts: null,
        open: false,
        disks: [],
        root: '',
        path: '',
        dirs: [],
        files: [],
        truncated: false,
        selected: null,
        afterMsg: '',
        busy: false,
        seq: 0
    };

    var el = null; // the DOM, created once

    function make(tag, cls, text) {
        var e = document.createElement(tag);
        if (cls) e.className = cls;
        if (text !== undefined && text !== null) e.textContent = text;
        return e;
    }

    function build() {
        if (el) return;
        el = {};
        el.overlay = make('div');
        el.box = make('div');
        el.overlay.appendChild(el.box);
        el.title = make('h3');
        el.box.appendChild(el.title);

        el.disk = make('div', 'pk-disk');
        var lab = make('label', null, ps5t('Drive'));
        lab.setAttribute('for', 'pkDisk');
        el.select = make('select');
        el.select.id = 'pkDisk';
        el.disk.appendChild(lab);
        el.disk.appendChild(el.select);
        el.box.appendChild(el.disk);

        el.crumbs = make('div', 'pk-crumbs');
        el.crumbs.id = 'pkCrumbs';
        el.box.appendChild(el.crumbs);

        el.list = make('div', 'pk-list');
        el.list.id = 'pkList';
        el.list.setAttribute('role', 'listbox');
        el.box.appendChild(el.list);

        el.newRow = make('div', 'pk-new');
        el.newRow.id = 'pkNew';
        el.newRow.style.display = 'none';
        el.name = make('input');
        el.name.id = 'pkName';
        el.name.type = 'text';
        el.name.maxLength = 100;
        el.name.setAttribute('autocomplete', 'off');
        el.name.setAttribute('autocapitalize', 'off');
        el.name.setAttribute('spellcheck', 'false');
        el.name.placeholder = ps5t('Name of the new folder');
        el.make = make('button', null, ps5t('Create'));
        el.make.id = 'pkMake';
        el.make.type = 'button';
        el.makeCancel = make('button', 'alt', ps5t('Cancel'));
        el.makeCancel.id = 'pkMakeCancel';
        el.makeCancel.type = 'button';
        el.newRow.appendChild(el.name);
        el.newRow.appendChild(el.make);
        el.newRow.appendChild(el.makeCancel);
        el.box.appendChild(el.newRow);

        el.msg = make('div', 'msg');
        el.msg.id = 'pkMsg';
        el.msg.style.minHeight = '1.4em';
        el.msg.style.marginTop = '8px';
        el.box.appendChild(el.msg);

        el.buttons = make('div', 'modal-btns ps5-modal-buttons');
        el.newBtn = make('button', 'alt pk-left', ps5t('New folder'));
        el.newBtn.id = 'pkNewBtn';
        el.newBtn.type = 'button';
        el.cancel = make('button', 'alt', ps5t('Cancel'));
        el.cancel.id = 'pkCancel';
        el.cancel.type = 'button';
        el.choose = make('button', null, ps5t('Choose this folder'));
        el.choose.id = 'pkChoose';
        el.choose.type = 'button';
        el.buttons.appendChild(el.newBtn);
        el.buttons.appendChild(el.cancel);
        el.buttons.appendChild(el.choose);
        el.box.appendChild(el.buttons);

        el.overlay.addEventListener('click', function (ev) {
            if (ev.target === el.overlay) close();
        });
        el.cancel.addEventListener('click', close);
        el.choose.addEventListener('click', choose);
        el.newBtn.addEventListener('click', function () {
            el.newRow.style.display = 'flex';
            el.name.value = '';
            el.name.focus();
        });
        el.makeCancel.addEventListener('click', function () {
            el.newRow.style.display = 'none';
            msg('');
        });
        el.make.addEventListener('click', makeFolder);
        el.name.addEventListener('keydown', function (ev) {
            if (ev.key === 'Enter' || ev.keyCode === 13) { ev.preventDefault(); makeFolder(); }
            if (ev.key === 'Escape' || ev.keyCode === 27) { el.newRow.style.display = 'none'; }
        });
        el.select.addEventListener('change', function () {
            if (el.select.value) load(el.select.value);
        });
        document.body.appendChild(el.overlay);
    }

    function msg(text, bad) {
        if (!el) return;
        el.msg.textContent = text || '';
        el.msg.className = 'msg' + (bad ? ' bad' : '');
    }

    function overlayClasses() {
        var o = state.opts;
        el.overlay.className = (o.overlayClass || 'ps5-modal') + ' ps5-picker-overlay';
        el.overlay.id = o.overlayId || 'ps5-picker';
        el.box.className = (o.boxClass || 'ps5-modal-box pk-card') + ' ps5-picker-box';
    }

    function setChooseState() {
        var o = state.opts;
        var entry = null;
        if (o.mode === 'file') {
            entry = state.selected;
        } else {
            entry = { path: state.path, isDir: true, name: '' };
        }
        var ok = !!entry && !state.busy;
        if (ok && typeof o.canChoose === 'function') ok = !!o.canChoose(entry);
        el.choose.disabled = !ok;
        if (!ok && o.cannotChooseText && entry && !state.busy) {
            msg(o.cannotChooseText);
        } else if (!state.busy && !el.msg.textContent) {
            msg('');
        }
    }

    function rowDir(name) {
        var r = make('div', 'pk-row');
        r.tabIndex = 0;
        r.setAttribute('role', 'option');
        r.innerHTML = FOLDER_ICO;
        r.appendChild(make('span', 'pk-name', name));
        function go() { load(state.path + '/' + name); }
        r.addEventListener('click', go);
        r.addEventListener('keydown', function (ev) {
            if (ev.key === 'Enter' || ev.keyCode === 13) { ev.preventDefault(); go(); }
        });
        return r;
    }

    function rowFile(f) {
        var r = make('div', 'pk-row pk-file-row');
        r.tabIndex = 0;
        r.setAttribute('role', 'option');
        r.innerHTML = FILE_ICO;
        r.appendChild(make('span', 'pk-name', f.name));
        r.appendChild(make('span', 'pk-size', size(f.size)));
        if (state.selected && state.selected.path === state.path + '/' + f.name) r.className += ' sel';
        function pick() {
            state.selected = { path: state.path + '/' + f.name, isDir: false, name: f.name, size: f.size };
            var rows = el.list.querySelectorAll('.pk-file-row');
            for (var i = 0; i < rows.length; i++) rows[i].className = rows[i].className.replace(/\s*\bsel\b/g, '');
            r.className += ' sel';
            msg('');
            setChooseState();
        }
        r.addEventListener('click', pick);
        r.addEventListener('keydown', function (ev) {
            if (ev.key === 'Enter' || ev.keyCode === 13) { ev.preventDefault(); pick(); }
        });
        return r;
    }

    function rowUp() {
        var r = make('div', 'pk-row up');
        r.tabIndex = 0;
        r.setAttribute('role', 'option');
        r.innerHTML = UP_ICO;
        r.appendChild(make('span', 'pk-name', ps5t('Back')));
        function go() {
            var p = state.path.substring(0, state.path.lastIndexOf('/'));
            load(p || state.root);
        }
        r.addEventListener('click', go);
        r.addEventListener('keydown', function (ev) {
            if (ev.key === 'Enter' || ev.keyCode === 13) { ev.preventDefault(); go(); }
        });
        return r;
    }

    function render() {
        var o = state.opts;
        el.title.textContent = o.title || '';
        // the crumbs: the disk root and the path below it
        el.crumbs.textContent = '';
        var root = state.root || '';
        var rel = state.path.substring(root.length).split('/').filter(function (x) { return !!x; });
        var acc = root;
        function crumb(label, path, cur) {
            var b = make('button', 'pk-crumb' + (cur ? ' cur' : ''), label);
            b.type = 'button';
            if (!cur) b.addEventListener('click', function () { load(path); });
            el.crumbs.appendChild(b);
        }
        function sep() { el.crumbs.appendChild(make('span', 'pk-sep', '›')); }
        crumb(root, root, rel.length === 0);
        for (var i = 0; i < rel.length; i++) {
            sep();
            acc += '/' + rel[i];
            crumb(rel[i], acc, i === rel.length - 1);
        }
        // the list
        el.list.textContent = '';
        if (state.path !== root) el.list.appendChild(rowUp());
        for (var d = 0; d < state.dirs.length; d++) el.list.appendChild(rowDir(state.dirs[d]));
        if (o.mode === 'file') {
            for (var f = 0; f < state.files.length; f++) el.list.appendChild(rowFile(state.files[f]));
        }
        if (!el.list.children.length || (el.list.children.length === 1 && state.path !== root)) {
            el.list.appendChild(make('div', 'pk-empty', o.mode === 'file' ? ps5t('No .torrent files here') : ps5t('No subfolders')));
        }
        if (state.truncated) {
            el.list.appendChild(make('div', 'pk-empty', o.mode === 'file' ? ps5t('The first 500 items are shown') : ps5t('The first 500 folders are shown')));
        }
        // the disk select (the root of the answer may be a resolved alias)
        for (var s = 0; s < el.select.options.length; s++) {
            if (el.select.options[s].value === state.root) el.select.value = state.root;
        }
        setChooseState();
        el.list.scrollTop = 0;
    }

    function load(path) {
        var o = state.opts;
        var seq = ++state.seq;
        state.busy = true;
        state.selected = null;
        setChooseState();
        el.choose.disabled = true;
        msg(ps5t('Loading…'));
        var p;
        try {
            p = o.list(path);
        } catch (e) {
            p = Promise.reject(e);
        }
        Promise.resolve(p).then(function (j) {
            if (seq !== state.seq) return;
            state.busy = false;
            if (!j || !j.dirs) {
                msg(ps5t('Could not open the folder'), true);
                return;
            }
            state.path = j.path || path;
            state.root = j.root || state.root;
            state.dirs = j.dirs || [];
            state.files = j.files || [];
            state.truncated = !!j.truncated;
            msg(state.afterMsg || '');
            state.afterMsg = '';
            render();
        }, function (err) {
            if (seq !== state.seq) return;
            state.busy = false;
            state.afterMsg = '';
            msg((err && (err.message || err.error)) || ps5t('Could not open the folder'), true);
        });
    }

    function makeFolder() {
        if (!state.opts || !state.opts.mkdir) return;
        var name = el.name.value;
        msg('');
        Promise.resolve(state.opts.mkdir(state.path, name)).then(function (j) {
            el.newRow.style.display = 'none';
            state.afterMsg = ps5t('Folder created');
            load((j && j.path) || state.path);
        }, function (err) {
            msg((err && (err.message || err.error)) || ps5t('Could not create the folder'), true);
        });
    }

    function choose() {
        var o = state.opts;
        if (!o) return;
        var entry;
        if (o.mode === 'file') {
            entry = state.selected;
        } else {
            entry = { path: state.path, isDir: true, name: '' };
        }
        if (!entry) return;
        if (typeof o.canChoose === 'function' && !o.canChoose(entry)) return;
        close();
        if (typeof o.onChoose === 'function') o.onChoose(entry);
    }

    function close() {
        if (!el || !state.open) return;
        state.open = false;
        state.seq++;
        el.overlay.style.display = 'none';
        document.removeEventListener('keydown', onKey, true);
    }

    function onKey(ev) {
        if (ev.key === 'Escape' || ev.keyCode === 27) {
            ev.preventDefault();
            close();
        }
    }

    function disksLabel(d) {
        var o = state.opts;
        var text = d.root + ' · ' + (KIND[d.kind] || d.kind || '');
        if (d.fs) text += ' (' + d.fs + ')';
        if (d.total) text += ', ' + ps5t('{1} free of {2}', [size(d.free), size(d.total)]);
        if (typeof o.diskBad === 'function' && o.diskBad(d)) text += ', ' + ps5t('not recommended');
        return text;
    }

    function defaultRoot(disks) {
        var pick = null;
        for (var i = 0; i < disks.length; i++) if (disks[i].root === '/mnt/ext1') pick = disks[i];
        if (!pick) for (var j = 0; j < disks.length; j++) if (disks[j].kind === 'ssd') { pick = disks[j]; break; }
        if (!pick) for (var k = 0; k < disks.length; k++) if (disks[k].kind === 'usb') { pick = disks[k]; break; }
        if (!pick && disks.length) pick = disks[disks.length - 1];
        return pick ? pick.root : '';
    }

    // Create the dialog (hidden) before it is opened: the settings page keeps the element on the page from the start.
    function preload(opts) {
        build();
        state.opts = opts || {};
        overlayClasses();
        el.overlay.style.display = 'none';
    }

    function open(opts) {
        build();
        if (state.open) close();
        state.opts = opts || {};
        state.selected = null;
        state.afterMsg = '';
        state.truncated = false;
        state.dirs = [];
        state.files = [];
        overlayClasses();
        if (state.opts.mkdir) {
            el.newBtn.style.display = '';
        } else {
            el.newBtn.style.display = 'none';
        }
        el.newRow.style.display = 'none';
        el.choose.textContent = state.opts.chooseLabel || (state.opts.mode === 'file' ? ps5t('Choose this file') : ps5t('Choose this folder'));
        el.title.textContent = state.opts.title || '';
        el.list.textContent = '';
        el.crumbs.textContent = '';
        msg(ps5t('Loading…'));
        el.overlay.style.display = 'flex';
        el.overlay.classList.add('on');
        state.open = true;
        document.addEventListener('keydown', onKey, true);
        el.choose.disabled = true;

        var disks = state.opts.disks;
        var p;
        try {
            p = typeof disks === 'function' ? disks() : disks;
        } catch (e) {
            p = Promise.reject(e);
        }
        Promise.resolve(p).then(function (list) {
            state.disks = list || [];
            el.select.textContent = '';
            for (var i = 0; i < state.disks.length; i++) {
                var d = state.disks[i];
                var o = document.createElement('option');
                o.value = d.root;
                o.textContent = disksLabel(d);
                el.select.appendChild(o);
            }
            var start = state.opts.startPath || '';
            var disk = null;
            for (var j = 0; j < state.disks.length; j++) {
                var r = state.disks[j].root;
                if (start === r || start.indexOf(r + '/') === 0) { disk = state.disks[j]; break; }
            }
            if (!disk) {
                start = defaultRoot(state.disks);
                el.select.value = start;
            } else {
                el.select.value = disk.root;
                start = start || disk.root;
            }
            state.root = el.select.value;
            load(start || el.select.value);
        }, function () {
            msg(ps5t('Could not open the folder'), true);
        });
    }

    window.ps5Picker = { open: open, preload: preload, close: close };
})();
