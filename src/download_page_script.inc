LR"JS((function () {
  'use strict';
  if (window !== window.top || location.protocol !== 'https:' || location.hostname !== 'osu.sayobot.cn') return;
  if (window.__ttpSayobotAdapter) return;
  window.__ttpSayobotAdapter = true;
  var maps = {}, pending = false, notice, noticeTimer;
  function toast(text) {
    if (!document.body) return;
    if (!notice) {
      notice = document.createElement('div'); notice.id = 'ttp-sayobot-notice';
      notice.style.cssText = 'position:fixed;bottom:20px;left:10%;max-width:80%;padding:10px 14px;background:#263746;color:white;border-radius:4px;z-index:2147483647;font:14px sans-serif;pointer-events:none;white-space:pre-wrap';
      document.body.appendChild(notice);
    }
    notice.textContent = text; notice.style.display = 'block';
    clearTimeout(noticeTimer); noticeTimer = setTimeout(function () { notice.style.display = 'none'; }, 4500);
  }
  function send(message) {
    message.type = 'ttp-sayobot';
    var text = JSON.stringify(message);
    if (window.chrome && chrome.webview) chrome.webview.postMessage(text);
    else location.href = 'ttpbrowser-action:' + encodeURIComponent(text);
  }
  if (window.chrome && chrome.webview) chrome.webview.addEventListener('message', function (e) {
    if (e.data && e.data.type === 'ttp-sayobot-result') toast(e.data.message);
  });
  window.__ttpSayobotResult = toast;
  // The site's preview player uses a detached new Audio(). Observe it without
  // changing play()'s behavior, returned promise, mute state or volume.
  var previews = [];
  function isPreview(media) {
    return /^https:\/\/cdnx\.sayobot\.cn:25225\/preview\/[1-9]\d*\.mp3(?:[?#]|$)/.test(media.src || media.currentSrc || '');
  }
  function rememberPreview(media) {
    if (!isPreview(media)) return;
    var active = [];
    for (var i = 0; i < previews.length; ++i)
      if (previews[i] !== media && !previews[i].paused && !previews[i].ended) active.push(previews[i]);
    active.push(media); previews = active;
  }
  function stopPreview() {
    // Use the site's visible pause control too, keeping its play icon in sync.
    var pauseButton = document.querySelector('.player-wrapper .action.pause');
    if (pauseButton) pauseButton.click();
    var media = document.querySelectorAll('audio,video');
    for (var i = 0; i < media.length; ++i) if (isPreview(media[i])) media[i].pause();
    for (var n = 0; n < previews.length; ++n) if (isPreview(previews[n])) previews[n].pause();
    previews = [];
  }
  if (window.HTMLMediaElement) {
    var originalPlay = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
      rememberPreview(this);
      return originalPlay.apply(this, arguments);
    };
    document.addEventListener('play', function (e) { rememberPreview(e.target); }, true);
  }
  function remember(xhr) {
    try {
      var url = xhr.responseURL || '', value = xhr.responseType === 'json' ? xhr.response : JSON.parse(xhr.responseText);
      if (/^https:\/\/api\.sayobot\.cn\/v2\/beatmapinfo(?:\?|$)/.test(url) && value && value.data && value.data.sid)
        maps[value.data.sid] = value.data;
    } catch (_) {}
  }
  var xhrOpen = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function () {
    this.addEventListener('load', function () { remember(this); });
    return xhrOpen.apply(this, arguments);
  };
  function play(detail) {
    var link = detail.querySelector('a[href*="preview.html"]'), match = link && /[?&]sid=(\d+).*?[?&]bid=(\d+)/.exec(link.href);
    if (!match) { toast('无法识别当前谱面，请重新打开详情。'); return; }
    if (pending) return;
    var sid = match[1], bid = match[2];
    function submit(data) {
      pending = false;
      var list = data && data.bid_data || [], entry;
      for (var i = 0; i < list.length; ++i) if (String(list[i].bid) === bid) { entry = list[i]; break; }
      if (!entry || !entry.audio) { toast('未找到当前难度的音频文件。'); return; }
      var parts = String(entry.audio).replace(/\\/g, '/').split('/');
      for (var n = 0; n < parts.length; ++n) { if (!parts[n] || parts[n] === '.' || parts[n] === '..') { toast('音频路径无效。'); return; } parts[n] = encodeURIComponent(parts[n]); }
      stopPreview();
      send({kind:'play', url:'https://dl.sayobot.cn/beatmaps/files/' + sid + '/' + parts.join('/'),
        title:String(data.title || ''), artist:String(data.artist || ''), tid:'sayobot:' + sid + ':' + bid});
    }
    if (maps[sid]) { submit(maps[sid]); return; }
    pending = true; toast('正在读取歌曲信息…');
    var xhr = new XMLHttpRequest(); xhr.open('GET', 'https://api.sayobot.cn/v2/beatmapinfo?0=' + sid, true); xhr.timeout = 15000;
    xhr.onload = function () { try { if (xhr.status !== 200) throw 0; submit(JSON.parse(xhr.responseText).data); } catch (_) { pending = false; toast('读取歌曲信息失败，请重试。'); } };
    xhr.onerror = xhr.ontimeout = function () { pending = false; toast('读取歌曲信息失败，请重试。'); }; xhr.send();
  }
  document.addEventListener('click', function (e) {
    var icon = e.target;
    while (icon && icon.tagName !== 'MAT-ICON') icon = icon.parentElement;
    if (icon && String(icon.textContent).replace(/\s/g, '') === 'cloud_download') {
      var card = icon;
      while (card && !/(^|\s)map-wrap(\s|$)/.test(card.className)) card = card.parentElement;
      var cover = card && card.querySelector('.img-wrap img'), id = cover && /\/beatmaps\/(\d+)\//.exec(cover.src);
      if (id) { e.preventDefault(); e.stopImmediatePropagation(); send({kind:'download',url:'https://txy1.sayobot.cn/beatmaps/download/full/' + id[1] + '?server=auto'}); return; }
    }
    var button = e.target;
    while (button && button.tagName !== 'BUTTON') button = button.parentElement;
    if (!button || !button.parentElement || !/(^|\s)actions(\s|$)/.test(button.parentElement.className)) return;
    var detail = button.parentElement;
    while (detail && !/(^|\s)map-detail-wrapper(\s|$)/.test(detail.className)) detail = detail.parentElement;
    if (!detail) return;
    var buttons = button.parentElement.querySelectorAll('button'), index = -1;
    for (var i = 0; i < buttons.length; ++i) if (buttons[i] === button) index = i;
    if (index === 0) { e.preventDefault(); e.stopImmediatePropagation(); play(detail); return; }
    if (index >= 1 && index <= 3) {
      var preview = detail.querySelector('a[href*="preview.html"]'), sid = preview && /[?&]sid=(\d+)/.exec(preview.href);
      if (sid) { e.preventDefault(); e.stopImmediatePropagation();
        send({kind:'download',url:'https://txy1.sayobot.cn/beatmaps/download/' + ['','full','novideo','mini'][index] + '/' + sid[1] + '?server=auto'}); }
    }
  }, true);
  // Site downloads use detached, programmatically clicked anchors. Consume
  // them before redirects can replace the original recognized package URL.
  var anchorClick = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function () {
    if (/^https:\/\/(?:txy1|dl)\.sayobot\.cn\/beatmaps\/download\/(?:full|novideo|mini)\/[1-9]\d*(?:\?|$)/.test(this.href)) {
      send({kind:'download',url:this.href}); return;
    }
    return anchorClick.call(this);
  };
})();)JS"
