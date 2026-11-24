/*
 * Web Server Lab: 버튼 → CGI 프로그램(/cgi-bin/lab) → 프록시(8080) → 백엔드.
 * 이 파일은 요청을 직접 보내지 않는다. 요청을 보내는 것은 CGI 프로세스이고,
 * 여기서는 CGI가 돌려준 결과와 프록시 로그를 읽어 화면에 그린다.
 */
(function () {
  'use strict';

  var CGI = '/cgi-bin/lab';
  var POS = {
    client: [110, 190],
    proxy: [435, 190],
    8001: [780, 70],
    8003: [780, 150],
    8002: [780, 250],
    ext: [780, 345]
  };
  var BACKENDS = [8001, 8003, 8002];

  var counts = { 8001: 0, 8003: 0, 8002: 0 };
  var up = { 8001: true, 8003: true, 8002: true };
  var logOffset = 0;
  var logTail = '';
  var busy = false;
  var current = 'reverse';

  function $(id) { return document.getElementById(id); }

  function call(params) {
    var query = Object.keys(params).map(function (k) {
      return encodeURIComponent(k) + '=' + encodeURIComponent(params[k]);
    }).join('&');
    return fetch(CGI + '?' + query, { cache: 'no-store' }).then(function (r) {
      if (!r.ok) { throw new Error('CGI 응답 ' + r.status); }
      return r.json();
    });
  }

  function sleep(ms) { return new Promise(function (res) { setTimeout(res, ms); }); }

  /* ---------- 장면(탭) ---------- */

  var SCENES = {
    reverse: {
      tab: 'Reverse Proxy',
      title: 'Reverse Proxy: 어디로 보낼지 프록시가 정한다',
      desc: '클라이언트는 요청 줄에 경로만 적는다. 프록시는 proxy.conf에서 경로에 맞는 규칙을 찾아 그 백엔드로 보낸다. 맞는 규칙이 없으면 404를 보낸다.',
      conf: true,
      controls: [
        { label: 'GET /api/', cls: 'primary', run: function () { return send({ mode: 'reverse', path: '/api/' }); } },
        { label: 'GET /static/', cls: 'primary', run: function () { return send({ mode: 'reverse', path: '/static/' }); } },
        { label: 'GET /img/a.png', cls: '', run: function () { return send({ mode: 'reverse', path: '/img/a.png' }); } },
        { hint: '/img/ 는 규칙이 없는 경로' }
      ]
    },
    rr: {
      tab: 'Round-Robin',
      title: 'Round-Robin: 백엔드가 둘이면 번갈아 고른다',
      desc: '/api/ 규칙에는 백엔드가 8001과 8003 두 개다. 프록시는 순번을 기억해 두고 요청마다 다음 백엔드를 고른다. 백엔드 칸의 숫자는 그 백엔드가 받은 요청 수다.',
      controls: [
        { label: 'GET /api/ 한 번', cls: 'primary', run: function () { return send({ mode: 'reverse', path: '/api/' }); } },
        { label: 'GET /api/ 여섯 번', cls: 'primary', run: function () { return sendMany(6); } },
        { label: '요청 수 지우기', cls: 'plain', run: function () { resetCounts(); return Promise.resolve(); } }
      ]
    },
    forward: {
      tab: 'Forward Proxy',
      title: 'Forward Proxy: 어디로 보낼지 클라이언트가 정한다',
      desc: '클라이언트가 요청 줄에 서버 주소까지 통째로 적는다. 프록시는 proxy.conf를 보지 않고, 요청 줄에서 주소와 포트를 꺼내 그 서버에 연결한다.',
      controls: [
        { label: 'GET http://127.0.0.1:8002/static/', cls: 'forward', run: function () { return send({ mode: 'forward', url: 'http://127.0.0.1:8002/static/' }); } },
        { label: 'GET http://127.0.0.1:8001/api/', cls: 'forward', run: function () { return send({ mode: 'forward', url: 'http://127.0.0.1:8001/api/' }); } },
        { sep: true },
        { input: 'forward-url', value: 'http://example.com/' },
        { label: '이 주소로 보내기', cls: 'forward plain', run: function () { return send({ mode: 'forward', url: $('forward-url').value.trim() }); } }
      ]
    },
    bad: {
      tab: '502 Bad Gateway',
      title: '502 Bad Gateway: 고른 백엔드가 꺼져 있을 때',
      desc: '백엔드를 끄면 프록시의 connect가 실패한다. 프록시는 응답을 만들 수 없으므로 클라이언트에게 502를 보낸다. 프록시는 꺼진 백엔드를 미리 알지 못해서 순번이 오면 그대로 고른다.',
      controls: [
        { toggle: 8001 }, { toggle: 8003 }, { toggle: 8002 },
        { sep: true },
        { label: 'GET /api/', cls: 'primary', run: function () { return send({ mode: 'reverse', path: '/api/' }); } },
        { label: 'GET /static/', cls: 'primary', run: function () { return send({ mode: 'reverse', path: '/static/' }); } }
      ]
    }
  };

  function buildTabs() {
    var nav = $('tabs');
    Object.keys(SCENES).forEach(function (key) {
      var b = document.createElement('button');
      b.type = 'button';
      b.setAttribute('role', 'tab');
      b.dataset.scene = key;
      b.textContent = SCENES[key].tab;
      b.addEventListener('click', function () { showScene(key); });
      nav.appendChild(b);
    });
  }

  function showScene(key) {
    current = key;
    var scene = SCENES[key];
    Array.prototype.forEach.call($('tabs').children, function (b) {
      b.setAttribute('aria-selected', b.dataset.scene === key ? 'true' : 'false');
    });
    $('scene-title').textContent = scene.title;
    $('scene-desc').textContent = scene.desc;
    $('conf').hidden = !scene.conf;

    var box = $('controls');
    box.textContent = '';
    scene.controls.forEach(function (c) {
      var el;
      if (c.sep) {
        el = document.createElement('span');
        el.className = 'sep';
      } else if (c.hint) {
        el = document.createElement('span');
        el.className = 'hint';
        el.textContent = c.hint;
      } else if (c.input) {
        el = document.createElement('input');
        el.id = c.input;
        el.type = 'text';
        el.value = c.value;
        el.setAttribute('aria-label', '보낼 주소 (http://로 시작)');
      } else if (c.toggle) {
        el = document.createElement('button');
        el.type = 'button';
        el.dataset.toggle = c.toggle;
        el.addEventListener('click', function () { guarded(function () { return toggleBackend(c.toggle); }); });
      } else {
        el = document.createElement('button');
        el.type = 'button';
        el.className = c.cls;
        el.textContent = c.label;
        el.addEventListener('click', function () { guarded(c.run); });
      }
      box.appendChild(el);
    });
    paintStatus();
  }

  /* 요청 하나가 끝나기 전에 다른 버튼이 눌리지 않게 한다 */
  function guarded(run) {
    if (busy) { return; }
    busy = true;
    setDisabled(true);
    Promise.resolve().then(run).catch(function (e) {
      showError(e.message);
    }).then(function () {
      busy = false;
      setDisabled(false);
    });
  }

  function setDisabled(flag) {
    Array.prototype.forEach.call($('controls').querySelectorAll('button'), function (b) { b.disabled = flag; });
  }

  /* ---------- 상태(켜짐/꺼짐)와 요청 수 ---------- */

  function paintStatus() {
    BACKENDS.forEach(function (port) {
      var node = $('node-' + port);
      node.classList.toggle('up', up[port]);
      node.classList.toggle('down', !up[port]);
      $('cnt-' + port).textContent = up[port] ? '요청 ' + counts[port] : '꺼짐 · 요청 ' + counts[port];
      var btn = $('controls').querySelector('[data-toggle="' + port + '"]');
      if (btn) {
        btn.textContent = up[port] ? '백엔드 ' + port + ' 끄기' : '백엔드 ' + port + ' 켜기';
        btn.className = 'plain ' + (up[port] ? 'off' : 'on');
      }
    });
  }

  function refreshStatus() {
    return call({ action: 'status' }).then(function (s) {
      s.backends.forEach(function (b) { up[b.port] = b.up; });
      var proxy = $('node-proxy');
      proxy.classList.toggle('up', s.proxy.up);
      proxy.classList.toggle('down', !s.proxy.up);
      var link = $('link-state');
      link.textContent = s.proxy.up ? '프록시 8080 실행 중' : '프록시 8080 꺼짐';
      link.className = 'link-state ' + (s.proxy.up ? 'ok' : 'bad');
      paintStatus();
    }).catch(function () {
      var link = $('link-state');
      link.textContent = 'CGI 프로그램에 연결할 수 없음';
      link.className = 'link-state bad';
    });
  }

  function toggleBackend(port) {
    return call({ action: up[port] ? 'stop' : 'start', port: port }).then(function (r) {
      if (!r.ok) { throw new Error(r.error); }
      up[port] = r.up;
      paintStatus();
    });
  }

  function resetCounts() {
    BACKENDS.forEach(function (p) { counts[p] = 0; });
    paintStatus();
  }

  /* ---------- 프록시 로그 ---------- */

  /* 로그 파일에서 새로 쌓인 줄을 읽어 화면에 붙이고, 그 줄들을 돌려준다 */
  function pullLog() {
    return call({ action: 'log', from: logOffset }).then(function (r) {
      if (!r.ok) { return []; }
      logOffset = r.next;
      var text = logTail + r.text;
      var lines = text.split('\n');
      logTail = lines.pop();
      var box = $('log');
      lines.forEach(function (line) {
        var div = document.createElement('div');
        div.textContent = line;
        if (line.indexOf('[Reverse]') === 0) { div.className = line.indexOf('404') > 0 ? 'bad' : 'reverse'; }
        else if (line.indexOf('[Forward]') === 0) { div.className = 'forward'; }
        else if (line.indexOf('connect') === 0 || line.indexOf('getaddrinfo') === 0) { div.className = 'bad'; }
        else { div.className = 'dim'; }
        box.appendChild(div);
      });
      while (box.childNodes.length > 300) { box.removeChild(box.firstChild); }
      box.scrollTop = box.scrollHeight;
      return lines;
    });
  }

  /* 로그 줄 "[Reverse] GET /api/ → 127.0.0.1:8001" 에서 프록시가 정한 목적지를 꺼낸다 */
  function readDecision(lines) {
    for (var i = lines.length - 1; i >= 0; i--) {
      var m = /^\[(Reverse|Forward)\] \S+ \S+ → (.+)$/.exec(lines[i]);
      if (m) {
        var dest = /^(\S+):(\d+)$/.exec(m[2]);
        return { kind: m[1], host: dest ? dest[1] : null, port: dest ? Number(dest[2]) : null, text: m[2] };
      }
    }
    return null;
  }

  /* ---------- 요청 보내기와 그림 ---------- */

  function send(params, fast) {
    params.action = 'send';
    var result;
    return call(params).then(function (r) {
      if (!r.ok) { throw new Error(r.error); }
      result = r;
      return pullLog();
    }).then(function (lines) {
      showExchange(result);
      return animate(result, readDecision(lines), fast);
    }).then(function () {
      return refreshStatus();
    });
  }

  function sendMany(n) {
    var chain = Promise.resolve();
    for (var i = 0; i < n; i++) {
      chain = chain.then(function () { return send({ mode: 'reverse', path: '/api/' }, true); });
    }
    return chain;
  }

  function showExchange(r) {
    var sent = $('sent');
    sent.className = 'mono';
    sent.textContent = '';
    var lines = r.sent.replace(/\r\n\r\n$/, '').split('\r\n');
    lines.forEach(function (line, i) {
      var span = document.createElement('span');
      span.textContent = line + '\n';
      if (i === 0) { span.className = 'first' + (r.mode === 'forward' ? ' forward' : ''); }
      sent.appendChild(span);
    });

    var got = $('received');
    got.className = 'mono';
    got.textContent = '';
    var head = r.headers.split('\r\n');
    head.forEach(function (line, i) {
      var span = document.createElement('span');
      span.textContent = line + '\n';
      if (i === 0) { span.className = r.status >= 200 && r.status < 400 ? 'status-ok' : 'status-bad'; }
      got.appendChild(span);
    });
    var body = document.createElement('span');
    body.textContent = '\n' + r.body + (r.body_truncated ? '\n… (뒤는 생략)' : '');
    got.appendChild(body);
    $('res-meta').textContent = r.bytes + '바이트 · ' + r.ms + 'ms';
  }

  function showError(message) {
    var got = $('received');
    got.className = 'mono';
    got.textContent = '';
    var span = document.createElement('span');
    span.className = 'status-bad';
    span.textContent = '요청을 보내지 못했다: ' + message;
    got.appendChild(span);
    $('res-meta').textContent = '';
  }

  function setChip(id, text, cls) {
    var el = $(id);
    el.textContent = text;
    el.setAttribute('class', 'chip ' + (cls || ''));
  }

  function clearMarks() {
    Array.prototype.forEach.call(document.querySelectorAll('.node'), function (n) {
      n.classList.remove('hit', 'fail', 'busy');
    });
    Array.prototype.forEach.call(document.querySelectorAll('.edges line'), function (l) {
      l.setAttribute('class', l.id === 'edge-ext' && $('node-ext').classList.contains('hidden') ? 'hidden' : '');
    });
  }

  /* 점을 from에서 to까지 ms 동안 옮긴다. stopAt(0~1)이 있으면 그 비율에서 멈춘다 */
  function moveDot(from, to, ms, stopAt) {
    var dot = $('dot');
    var end = stopAt === undefined ? 1 : stopAt;
    return new Promise(function (resolve) {
      var start = null;
      function step(now) {
        if (start === null) { start = now; }
        var t = Math.min((now - start) / ms, 1) * end;
        dot.setAttribute('cx', from[0] + (to[0] - from[0]) * t);
        dot.setAttribute('cy', from[1] + (to[1] - from[1]) * t);
        if (now - start < ms) { requestAnimationFrame(step); } else { resolve(); }
      }
      requestAnimationFrame(step);
    });
  }

  function markEdge(id, cls) {
    $(id).setAttribute('class', 'active ' + (cls || ''));
  }

  function animate(r, decision, fast) {
    var hop = fast ? 170 : 480;
    var pause = fast ? 60 : 260;
    var dot = $('dot');
    var ok = r.status >= 200 && r.status < 400;

    /* 프록시가 정한 목적지가 그림의 백엔드인지, 그림에 없는 다른 서버인지 */
    var target = null;
    if (decision && decision.port !== null) {
      var local = decision.host === '127.0.0.1' || decision.host === 'localhost';
      target = local && POS[decision.port] ? String(decision.port) : 'ext';
    }
    var ext = $('node-ext');
    ext.classList.toggle('hidden', target !== 'ext');
    /* 그림에 없는 서버로 갈 때만 그림 아래쪽의 '다른 서버' 칸까지 보이게 한다 */
    $('stage').setAttribute('viewBox', target === 'ext' ? '0 20 900 370' : '0 20 900 270');
    if (target === 'ext') { $('ext-name').textContent = decision.text; }
    clearMarks();
    setChip('client-chip', '', '');
    setChip('proxy-note', '', '');

    dot.setAttribute('class', 'dot');
    dot.setAttribute('cx', POS.client[0]);
    dot.setAttribute('cy', POS.client[1]);

    var edgeId = target ? 'edge-' + target : null;
    var nodeId = target ? 'node-' + target : null;

    markEdge('edge-client');
    return moveDot(POS.client, POS.proxy, hop).then(function () {
      $('node-proxy').classList.add('busy');
      if (decision) {
        var kind = decision.kind === 'Forward' ? 'forward' : 'reverse';
        setChip('proxy-note', '[' + decision.kind + '] → ' + decision.text, decision.port === null ? 'bad' : kind);
      } else {
        setChip('proxy-note', '프록시 로그에 목적지 줄이 없음', 'bad');
      }
      return sleep(pause);
    }).then(function () {
      if (!target) { return null; }
      markEdge(edgeId);
      if (r.status === 502) {
        /* 연결하지 못했다: 백엔드 앞에서 멈춘다 */
        return moveDot(POS.proxy, POS[target], hop, 0.82).then(function () {
          $(nodeId).classList.add('fail');
          markEdge(edgeId, 'err');
          dot.setAttribute('class', 'dot err');
          return sleep(pause);
        }).then(function () {
          var stopped = [Number(dot.getAttribute('cx')), Number(dot.getAttribute('cy'))];
          return moveDot(stopped, POS.proxy, hop);
        });
      }
      return moveDot(POS.proxy, POS[target], hop).then(function () {
        $(nodeId).classList.add('hit');
        if (counts[target] !== undefined) {
          counts[target] += 1;
          paintStatus();
        }
        dot.setAttribute('class', 'dot ' + (ok ? 'res' : 'err'));
        markEdge(edgeId, ok ? 'res' : 'err');
        return sleep(pause);
      }).then(function () {
        return moveDot(POS[target], POS.proxy, hop);
      });
    }).then(function () {
      dot.setAttribute('class', 'dot ' + (ok ? 'res' : 'err'));
      markEdge('edge-client', ok ? 'res' : 'err');
      return moveDot(POS.proxy, POS.client, hop);
    }).then(function () {
      $('node-proxy').classList.remove('busy');
      setChip('client-chip', r.status_line.replace(/^HTTP\/\d\.\d /, ''), ok ? 'ok' : 'bad');
      dot.setAttribute('class', 'dot hidden');
    });
  }

  /* ---------- 시작 ---------- */

  buildTabs();
  showScene(current);
  call({ action: 'conf' }).then(function (r) {
    if (!r.ok) { return; }
    $('conf').textContent = 'proxy.conf\n' + r.text.split('\n').filter(function (line) {
      return line.trim() !== '' && line.trim().charAt(0) !== '#';
    }).join('\n');
  }).catch(function () {});
  refreshStatus();
  pullLog().catch(function () {});
  setInterval(function () { if (!busy) { refreshStatus(); pullLog().catch(function () {}); } }, 2000);
})();
