/*
 * Web Server Lab: 버튼 → CGI 프로그램(/cgi-bin/lab) → 웹 서버 또는 프록시.
 * 이 파일은 요청을 직접 보내지 않는다. 요청을 보내는 것은 CGI 프로세스이고,
 * 여기서는 CGI가 돌려준 결과(응답, 걸린 시간, /proc 에서 읽은 fd 표)와 각 프로세스의 로그 파일을 읽어 화면에 그린다.
 */
(function () {
  'use strict';

  var CGI = '/cgi-bin/lab';
  var POS = {
    client: [110, 190],
    proxy: [435, 190],
    direct: [705, 190],
    8001: [780, 70],
    8003: [780, 150],
    8002: [780, 235],
    8004: [780, 315],
    ext: [780, 405]
  };
  var PROXY_BACKENDS = [8001, 8003, 8002, 8004];
  var PROCESS = {
    proxy: { title: '프록시 8080', cmd: './build/proxy 8080 demo/lab/proxy.conf' },
    8001: { title: '백엔드 8001', cmd: './build/server 8001 demo/proxy/backend1' },
    8003: { title: '백엔드 8003', cmd: './build/server 8003 demo/proxy/backend3' },
    8002: { title: '백엔드 8002', cmd: './build/server 8002 demo/proxy/backend2' },
    8004: { title: '웹 서버 8004', cmd: './build/server 8004 www' },
    8005: { title: '웹 서버 8005 (fork 없음)', cmd: './build/server-nofork 8005 www' }
  };

  var counts = { 8001: 0, 8003: 0, 8002: 0, 8004: 0 };
  var up = { 8001: true, 8003: true, 8002: true, 8004: true, 8005: true };
  var logs = {};      /* 이름 → { offset, tail, box } : 지금 화면에 있는 로그 칸 */
  var busy = false;
  var current = null;
  var directPort = 8004;

  function $(id) { return document.getElementById(id); }

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) { node.className = cls; }
    if (text !== undefined) { node.textContent = text; }
    return node;
  }

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

  /* 요청 글자를 만든다. 줄 끝은 CRLF. HTTP/1.1이면 Host 헤더를 넣는다 */
  function http(method, target, opt) {
    opt = opt || {};
    var version = opt.version || 'HTTP/1.0';
    var lines = [method + ' ' + target + ' ' + version];
    if (version === 'HTTP/1.1' && !opt.noHost) { lines.push('Host: localhost'); }
    (opt.headers || []).forEach(function (h) { lines.push(h); });
    if (opt.body !== undefined) { lines.push('Content-Length: ' + new TextEncoder().encode(opt.body).length); }
    return lines.join('\r\n') + '\r\n\r\n' + (opt.body || '');
  }

  /* ---------- 장면 ---------- */

  function get(port, target) { return function () { return send(port, http('GET', target)); }; }

  var STATUS_CASES = [
    { code: 200, to: 8004, raw: http('GET', '/index.html'), why: '200 OK: 파일 www/index.html이 있어서 그 내용을 본문으로 보냈다.' },
    { code: 400, to: 8004, raw: http('GET', '/', { version: 'HTTP/1.1', noHost: true }), why: '400 Bad Request: HTTP/1.1 요청인데 Host 헤더가 없다. HTTP/1.1은 Host 헤더가 반드시 있어야 한다.' },
    { code: 403, to: 8004, raw: http('GET', '/../Makefile'), why: '403 Forbidden: 경로에 ".."이 있어 정적 파일 폴더(www) 밖의 파일을 가리킨다. 웹 서버 프로세스가 파일을 열지 않고 거절했다.' },
    { code: 404, to: 8004, raw: http('GET', '/nope.html'), why: '404 Not Found: www/nope.html 파일이 없다.' },
    { code: 405, to: 8004, raw: http('POST', '/index.html', { headers: ['Content-Length: 0'] }), why: '405 Method Not Allowed: /index.html은 정적 파일이라 GET만 된다. 응답의 Allow 헤더가 되는 메서드를 알려 준다.' },
    { code: 411, to: 8004, raw: http('POST', '/cgi-bin/echo'), why: '411 Length Required: POST인데 Content-Length 헤더가 없다. 웹 서버 프로세스는 본문을 몇 바이트 읽어야 할지 알 수 없다.' },
    { code: 413, to: 8004, raw: http('POST', '/cgi-bin/echo', { headers: ['Content-Length: 2000000'] }), why: '413 Content Too Large: Content-Length가 2,000,000이다. 웹 서버가 받는 본문은 1MiB(1,048,576바이트)까지다. 본문을 읽기 전에 거절했다.' },
    { code: 414, to: 8004, raw: http('GET', '/{{a5000}}'), why: '414 URI Too Long: 경로가 5,000바이트다(a를 5,000개). 경로를 담는 버퍼는 4,096바이트다.' },
    { code: 431, to: 8004, raw: http('GET', '/', { headers: ['X-Long: {{a9000}}'] }), why: '431 Request Header Fields Too Large: 헤더 한 줄이 9,000바이트다. 요청 줄과 헤더를 받는 버퍼는 8,192바이트다.' },
    { code: 500, to: 8004, raw: http('GET', '/cgi-bin/fail'), why: '500 Internal Server Error: CGI 프로그램 fail이 출력 없이 실패(종료 값 1)로 끝났다. 웹 서버 프로세스가 응답을 만들 수 없다.' },
    { code: 501, to: 8004, raw: http('PUT', '/index.html'), why: '501 Not Implemented: PUT 메서드는 이 웹 서버가 구현하지 않았다. 구현한 메서드는 GET과 POST다.' },
    { code: 502, to: 8080, raw: http('GET', '/down/'), why: '502 Bad Gateway: 이 요청만 프록시(8080)로 보냈다. 규칙 /down/의 백엔드 127.0.0.1:9에는 서버가 없어서 프록시 프로세스의 connect가 실패했다.' },
    { code: 505, to: 8004, raw: http('GET', '/', { version: 'HTTP/2.0' }), why: '505 HTTP Version Not Supported: 요청 줄의 버전이 HTTP/2.0이다. 이 웹 서버는 HTTP/1.0과 HTTP/1.1만 읽는다.' }
  ];

  var GROUPS = [
    {
      name: '소켓과 TCP',
      scenes: [
        {
          key: 'fd', tab: 'Socket과 fd', stage: 'direct', port: 8004,
          title: 'Socket과 fd: 듣기 소켓 하나, 연결마다 연결 소켓 하나',
          controls: [
            { label: '연결 하나 만들고 fd 표 읽기', cls: 'primary plain', run: function () { return runFds(false); } },
            { toggle: 8004 },
            { hint: '웹 서버를 껐다 켜면 로그에 socket, bind, listen 줄이 새로 찍힌다' }
          ],
          panes: [{ log: 8004 }, { fd: true }]
        },
        {
          key: 'tcp', tab: 'TCP Handshake와 레이어', badge: '준비 중', shell: true,
          title: 'TCP Handshake와 레이어: HTTP 요청 하나가 TCP, IP에 실려 가는 모습',
          controls: []
        }
      ]
    },
    {
      name: 'HTTP',
      scenes: [
        {
          key: 'get', tab: 'HTTP GET', stage: 'direct', port: 8004,
          title: 'HTTP GET: 정해진 글자 형식으로 파일을 달라고 한다',
          controls: [
            { label: 'GET /index.html', cls: 'primary', run: get(8004, '/index.html') },
            { label: 'GET /style.css', cls: 'primary', run: get(8004, '/style.css') },
            { label: 'GET /', cls: 'primary', run: get(8004, '/') },
            { label: 'GET /nope.html', cls: '', run: get(8004, '/nope.html') },
            { hint: '/ 는 index.html로 바뀐다 · /nope.html은 없는 파일' }
          ],
          panes: [{ log: 8004 }]
        },
        {
          key: 'status', tab: '상태 코드', stage: 'direct', port: 8004,
          title: '상태 코드: 요청이 어떻게 됐는지 숫자 세 자리로 알려 준다',
          controls: STATUS_CASES.map(function (c) {
            var cls = 'code ' + (c.code < 300 ? 'c2' : c.code < 500 ? 'c4' : 'c5');
            return { label: String(c.code), cls: cls, run: function () { return send(c.to, c.raw, { why: c.why }); } };
          }),
          panes: [{ log: 8004 }, { log: 'proxy' }]
        },
        {
          key: 'post', tab: 'POST', stage: 'direct', port: 8004,
          title: 'POST: 요청에 본문을 실어 서버로 데이터를 보낸다',
          controls: [
            { input: 'post-body', value: 'msg=hello&from=lab', label: '보낼 본문' },
            { label: 'POST /cgi-bin/echo', cls: 'primary', run: function () { return send(8004, http('POST', '/cgi-bin/echo', { body: $('post-body').value })); } },
            { label: 'Content-Length 없이 POST', cls: '', run: function () { return send(8004, http('POST', '/cgi-bin/echo')); } }
          ],
          panes: [{ log: 8004 }]
        },
        {
          key: 'http11', tab: 'HTTP/1.0과 HTTP/1.1', badge: '구현 전', shell: true,
          title: 'HTTP/1.0과 HTTP/1.1: 요청마다 새 연결과 연결 재사용 (keep-alive)',
          controls: []
        }
      ]
    },
    {
      name: '동적 요청과 동시성',
      scenes: [
        {
          key: 'cgi', tab: '정적과 동적 (CGI)', stage: 'direct', port: 8004,
          title: '정적과 동적 (CGI): 파일을 읽어 보내기와 프로그램을 실행해 보내기',
          controls: [
            { label: '정적: GET /index.html', cls: 'primary', run: get(8004, '/index.html') },
            { label: '동적: GET /cgi-bin/hello?name=somang', cls: 'primary', run: get(8004, '/cgi-bin/hello?name=somang') },
            { label: 'CGI 실행 중의 프로세스와 fd 읽기 (3초)', cls: 'plain', run: function () { return runFds(true); } }
          ],
          panes: [{ log: 8004 }, { fd: true }]
        },
        {
          key: 'fork', tab: 'fork 있음과 없음', stage: 'direct', port: 8004,
          title: 'fork 있음과 없음: 느린 요청 하나가 다른 클라이언트를 기다리게 하는가',
          controls: [
            { label: 'fork 있음 (8004): 느린 요청 + 빠른 요청', cls: 'primary plain', run: function () { return runRace(8004); } },
            { label: 'fork 없음 (8005): 느린 요청 + 빠른 요청', cls: 'plain', run: function () { return runRace(8005); } }
          ],
          panes: [{ log: 8004 }, { log: 8005 }]
        }
      ]
    },
    {
      name: 'Reverse Proxy',
      scenes: [
        {
          key: 'reverse', tab: 'Reverse Proxy', stage: 'proxy',
          title: 'Reverse Proxy: 어디로 보낼지 프록시가 정한다',
          controls: [
            { label: 'GET /api/', cls: 'primary', run: get(8080, '/api/') },
            { label: 'GET /static/', cls: 'primary', run: get(8080, '/static/') },
            { label: 'GET /cgi-bin/hello?via=proxy', cls: 'primary', run: get(8080, '/cgi-bin/hello?via=proxy') },
            { label: 'GET /img/a.png', cls: '', run: get(8080, '/img/a.png') },
            { hint: '/img/ 는 규칙이 없는 경로' }
          ],
          panes: [{ conf: true }, { log: 'proxy' }, { log: 8001 }, { log: 8002 }]
        },
        {
          key: 'rr', tab: 'Round-Robin', stage: 'proxy',
          title: 'Round-Robin: 백엔드가 둘이면 번갈아 고른다',
          controls: [
            { label: 'GET /api/ 한 번', cls: 'primary', run: get(8080, '/api/') },
            { label: 'GET /api/ 여섯 번', cls: 'primary', run: function () { return sendMany(6); } },
            { label: '요청 수 지우기', cls: 'plain', run: function () { resetCounts(); return Promise.resolve(); } }
          ],
          panes: [{ log: 'proxy' }, { log: 8001 }, { log: 8003 }]
        },
        {
          key: 'wrr', tab: '가중치 Round-Robin', badge: '구현 전', shell: true,
          title: '가중치 Round-Robin: 성능이 좋은 백엔드에 더 많이 보낸다',
          controls: []
        },
        {
          key: 'bad', tab: '502 Bad Gateway', stage: 'proxy',
          title: '502 Bad Gateway: 고른 백엔드가 꺼져 있을 때',
          controls: [
            { toggle: 8001 }, { toggle: 8003 }, { toggle: 8002 },
            { sep: true },
            { label: 'GET /api/', cls: 'primary', run: get(8080, '/api/') },
            { label: 'GET /static/', cls: 'primary', run: get(8080, '/static/') }
          ],
          panes: [{ log: 'proxy' }, { log: 8001 }, { log: 8003 }]
        },
        {
          key: 'health', tab: 'Health Check', badge: '구현 전', shell: true,
          title: 'Health Check: 꺼진 백엔드를 미리 알아내 순번에서 뺀다',
          controls: []
        }
      ]
    },
    {
      name: 'Forward Proxy',
      scenes: [
        {
          key: 'forward', tab: 'Forward Proxy', stage: 'proxy',
          title: 'Forward Proxy: 어디로 보낼지 클라이언트가 정한다',
          controls: [
            { label: 'GET http://127.0.0.1:8002/static/', cls: 'forward', run: get(8080, 'http://127.0.0.1:8002/static/') },
            { label: 'GET http://127.0.0.1:8001/api/', cls: 'forward', run: get(8080, 'http://127.0.0.1:8001/api/') },
            { sep: true },
            { input: 'forward-url', value: 'http://example.com/', label: '보낼 주소 (http://로 시작)' },
            { label: '이 주소로 보내기', cls: 'forward plain', run: function () { return send(8080, http('GET', $('forward-url').value.trim())); } }
          ],
          panes: [{ log: 'proxy' }, { log: 8001 }, { log: 8002 }]
        },
        {
          key: 'allow', tab: '허용 목록', stage: 'proxy',
          title: '허용 목록: 허용한 서버로만 나가게 한다',
          controls: [
            { label: '허용: http://127.0.0.1:8002/static/', cls: 'forward', run: get(8080, 'http://127.0.0.1:8002/static/') },
            { label: '허용: http://127.0.0.1:8001/api/', cls: 'forward', run: get(8080, 'http://127.0.0.1:8001/api/') },
            { label: '목록에 없음: http://127.0.0.1:8004/index.html', cls: '', run: get(8080, 'http://127.0.0.1:8004/index.html') },
            { label: '목록에 없음: http://127.0.0.1:8003/api/', cls: '', run: get(8080, 'http://127.0.0.1:8003/api/') }
          ],
          panes: [{ conf: true, bottom: true }, { log: 'proxy' }, { log: 8002 }, { log: 8004 }]
        }
      ]
    },
    {
      name: '프록시 부가 기능',
      scenes: [
        {
          key: 'xff', tab: 'X-Forwarded-For', badge: '구현 전', shell: true,
          title: 'X-Forwarded-For: 백엔드에게 원래 클라이언트의 주소를 알려 준다',
          controls: []
        },
        {
          key: 'cache', tab: 'Response Caching', badge: '구현 전', shell: true,
          title: 'Response Caching: 같은 요청은 백엔드에 다시 묻지 않는다',
          controls: []
        },
        {
          key: 'statuspage', tab: '/status', badge: '구현 전', shell: true,
          title: '/status: 프록시가 자기 상태를 응답으로 알려 준다',
          controls: []
        }
      ]
    }
  ];

  var SCENES = {};
  GROUPS.forEach(function (g, gi) {
    g.scenes.forEach(function (s) { s.group = gi; SCENES[s.key] = s; });
  });

  function buildCats() {
    var nav = $('cats');
    GROUPS.forEach(function (g, gi) {
      var b = el('button');
      b.type = 'button';
      b.dataset.group = gi;
      b.appendChild(el('span', 'n', String(gi + 1)));
      b.appendChild(document.createTextNode(g.name));
      b.addEventListener('click', function () { showScene(g.scenes[0].key); });
      nav.appendChild(b);
    });
  }

  function showScene(key) {
    if (busy) { return; }
    var scene = SCENES[key];
    current = scene;
    try { localStorage.setItem('lab-scene', key); } catch (e) { /* 저장하지 못해도 화면은 그대로 동작한다 */ }

    Array.prototype.forEach.call($('cats').children, function (b) {
      b.setAttribute('aria-expanded', Number(b.dataset.group) === scene.group ? 'true' : 'false');
    });
    var tabs = $('tabs');
    tabs.textContent = '';
    GROUPS[scene.group].scenes.forEach(function (s) {
      var b = el('button', '', s.tab);
      b.type = 'button';
      if (s.key === key) { b.setAttribute('aria-current', 'true'); }
      if (s.badge) { b.appendChild(el('span', 'tag', s.badge)); }
      b.addEventListener('click', function () { showScene(s.key); });
      tabs.appendChild(b);
    });

    $('scene-title').textContent = scene.title;
    $('scene-badge').hidden = !scene.badge;
    $('scene-badge').textContent = scene.badge === '준비 중' ? '준비 중' : '구현 전: 코드를 구현하면 채운다';
    $('why').hidden = true;
    $('main').className = scene.shell ? 'shell' : '';

    buildControls(scene);
    resetStage(scene);
    buildPanes(scene);
    paintStatus();
    pullLogs().catch(function () {});
  }

  function buildControls(scene) {
    var box = $('controls');
    box.textContent = '';
    scene.controls.forEach(function (c) {
      var node;
      if (c.sep) {
        node = el('span', 'sep');
      } else if (c.hint) {
        node = el('span', 'hint', c.hint);
      } else if (c.input) {
        node = el('input');
        node.id = c.input;
        node.type = 'text';
        node.value = c.value;
        node.setAttribute('aria-label', c.label);
      } else if (c.toggle) {
        node = el('button');
        node.type = 'button';
        node.dataset.toggle = c.toggle;
        node.addEventListener('click', function () { guarded(function () { return toggleBackend(c.toggle); }); });
      } else {
        node = el('button', c.cls, c.label);
        node.type = 'button';
        node.addEventListener('click', function () { guarded(c.run); });
      }
      box.appendChild(node);
    });
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

  /* ---------- 터미널 칸 ---------- */

  function buildPanes(scene) {
    var box = $('terms');
    box.textContent = '';
    logs = {};
    (scene.panes || []).forEach(function (p) {
      var term = el('div', 'term');
      var h = el('h3');
      var screen = el('div', 'screen');
      if (p.log) {
        var info = PROCESS[p.log];
        h.appendChild(el('span', 'who', info.title));
        h.appendChild(el('span', 'cmd', '$ ' + info.cmd));
        term.dataset.proc = p.log;
        logs[p.log] = { offset: -2500, tail: '', box: screen, first: true };
      } else if (p.fd) {
        h.appendChild(el('span', 'who', 'fd 표'));
        h.appendChild(el('span', 'cmd', '$ ls -l /proc/프로세스 번호/fd'));
        screen.id = 'fd-screen';
        screen.appendChild(el('div', 'empty', '버튼을 누르면 웹 서버 프로세스의 fd 표를 읽어 온다'));
      } else if (p.conf) {
        h.appendChild(el('span', 'who', '설정 파일'));
        h.appendChild(el('span', 'cmd', '$ cat demo/lab/proxy.conf'));
        call({ action: 'conf' }).then(function (r) {
          if (!r.ok) { return; }
          r.text.replace(/\n+$/, '').split('\n').forEach(function (line) {
            var cls = line.charAt(0) === '#' ? 'note' : line.indexOf('allow') === 0 ? 'forward' : line.charAt(0) === '/' ? 'reverse' : '';
            screen.appendChild(el('div', cls, line || ' '));
          });
          screen.scrollTop = p.bottom ? screen.scrollHeight : 0;
        }).catch(function () {});
      }
      term.appendChild(h);
      term.appendChild(screen);
      box.appendChild(term);
    });
  }

  function logClass(line) {
    if (line.indexOf('[Reverse]') === 0) { return line.indexOf('(404)') > 0 ? 'bad' : 'reverse'; }
    if (line.indexOf('[Forward]') === 0) { return line.indexOf('(403)') > 0 ? 'bad' : 'forward'; }
    if (line.indexOf('connect') === 0 || line.indexOf('getaddrinfo') === 0) { return 'bad'; }
    var m = /^응답: (\d+)/.exec(line);
    if (m) { return Number(m[1]) < 400 ? 'status-ok' : 'status-bad'; }
    if (line.indexOf('요청 줄:') === 0) { return 'first'; }
    if (/^(socket fd|bind 성공|listen 중|프록시 listen)/.test(line)) { return 'good'; }
    return '';
  }

  /* 화면에 있는 로그 칸마다 로그 파일에서 새로 쌓인 줄을 읽어 붙인다. 이름 → 새 줄들 */
  function pullLogs() {
    var names = Object.keys(logs);
    var fresh = {};
    return Promise.all(names.map(function (name) {
      var state = logs[name];
      return call({ action: 'log', name: name, from: state.offset }).then(function (r) {
        if (!r.ok || logs[name] !== state) { return; }
        state.offset = r.next;
        var text = state.tail + r.text;
        var lines = text.split('\n');
        state.tail = lines.pop();
        /* 처음에는 파일 끝의 일부만 읽으므로, 잘린 첫 줄은 버린다 */
        if (state.first) {
          state.first = false;
          if (lines.length > 1) { lines.shift(); }
          lines = lines.slice(-12);
        } else {
          Array.prototype.forEach.call(state.box.querySelectorAll('.new'), function (n) { if (lines.length) { n.classList.remove('new'); } });
          fresh[name] = lines;
        }
        lines.forEach(function (line) {
          var div = el('div', logClass(line), line);
          if (fresh[name]) { div.classList.add('new'); }
          state.box.appendChild(div);
        });
        while (state.box.childNodes.length > 200) { state.box.removeChild(state.box.firstChild); }
        state.box.scrollTop = state.box.scrollHeight;
      }).catch(function () {});
    })).then(function () { return fresh; });
  }

  /* 프록시 로그 줄 "[Reverse] GET /api/ → 127.0.0.1:8001" 에서 프록시가 정한 목적지를 꺼낸다 */
  function readDecision(lines) {
    for (var i = (lines || []).length - 1; i >= 0; i--) {
      var m = /^\[(Reverse|Forward)\] \S+ \S+ → (.+)$/.exec(lines[i]);
      if (m) {
        var dest = /^(\S+):(\d+)$/.exec(m[2]);
        return { kind: m[1], host: dest ? dest[1] : null, port: dest ? Number(dest[2]) : null, text: m[2] };
      }
    }
    return null;
  }

  /* ---------- 상태(켜짐/꺼짐)와 요청 수 ---------- */

  function paintStatus() {
    PROXY_BACKENDS.forEach(function (port) {
      var node = $('node-' + port);
      node.classList.toggle('up', up[port]);
      node.classList.toggle('down', !up[port]);
      $('cnt-' + port).textContent = up[port] ? '요청 ' + counts[port] : '꺼짐 · 요청 ' + counts[port];
    });
    var direct = $('node-direct');
    direct.classList.toggle('up', up[directPort]);
    direct.classList.toggle('down', !up[directPort]);
    Array.prototype.forEach.call($('controls').querySelectorAll('[data-toggle]'), function (btn) {
      var port = btn.dataset.toggle;
      var name = PROCESS[port].title;
      btn.textContent = up[port] ? name + ' 끄기' : name + ' 켜기';
      btn.className = 'plain ' + (up[port] ? 'off' : 'on');
    });
    Array.prototype.forEach.call($('terms').children, function (term) {
      var proc = term.dataset.proc;
      term.classList.toggle('down', proc !== undefined && proc !== 'proxy' && !up[proc]);
    });
  }

  function refreshStatus() {
    return call({ action: 'status' }).then(function (s) {
      s.backends.forEach(function (b) { up[b.port] = b.up; });
      var proxy = $('node-proxy');
      proxy.classList.toggle('up', s.proxy.up);
      proxy.classList.toggle('down', !s.proxy.up);
      var link = $('link-state');
      var downs = s.backends.filter(function (b) { return !b.up; }).map(function (b) { return b.port; });
      if (!s.proxy.up) { downs.unshift('프록시 8080'); }
      link.textContent = downs.length ? '꺼진 프로세스: ' + downs.join(', ') : '프로세스 6개 실행 중';
      link.className = 'link-state ' + (downs.length ? 'bad' : 'ok');
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
      return pullLogs();
    });
  }

  function resetCounts() {
    PROXY_BACKENDS.forEach(function (p) { counts[p] = 0; });
    paintStatus();
  }

  /* ---------- 보낸 요청과 받은 응답 칸 ---------- */

  /* 같은 글자가 길게 이어지는 부분은 줄여서 보여 준다 */
  function shorten(line) {
    return line.replace(/(.)\1{60,}/g, function (run, ch) { return ch + ch + ch + '…(' + ch + ' 가 계속 이어짐)…'; });
  }

  function fillSent(box, r) {
    var parts = r.sent.split('\r\n\r\n');
    var head = parts.shift().split('\r\n');
    var body = parts.join('\r\n\r\n');
    head.forEach(function (line, i) {
      var cls = '';
      if (i === 0) { cls = 'first' + (/^\S+ http:\/\//.test(line) ? ' forward' : ''); }
      else if (/^content-length:/i.test(line)) { cls = 'len'; }
      box.appendChild(el('span', cls, shorten(line) + '\n'));
    });
    if (!r.sent_truncated || body) { box.appendChild(el('span', 'cut', '(빈 줄)\n')); }
    if (body) { box.appendChild(el('span', 'payload', body)); box.appendChild(document.createTextNode('\n')); }
    if (r.sent_truncated) { box.appendChild(el('span', 'cut', '… 요청 전체는 ' + r.sent_bytes + '바이트\n')); }
  }

  function fillReceived(box, r) {
    r.headers.split('\r\n').forEach(function (line, i) {
      var cls = '';
      if (i === 0) { cls = r.status >= 200 && r.status < 400 ? 'status-ok' : 'status-bad'; }
      else if (/^content-length:/i.test(line)) { cls = 'len'; }
      box.appendChild(el('span', cls, line + '\n'));
    });
    box.appendChild(el('span', 'cut', '(빈 줄)\n'));
    box.appendChild(el('span', '', r.body + (r.body_truncated ? '\n' : '')));
    if (r.body_truncated) { box.appendChild(el('span', 'cut', '… 본문 뒤는 생략')); }
  }

  function showExchange(r) {
    var sent = $('sent');
    sent.className = '';
    sent.textContent = '';
    fillSent(sent, r);
    var got = $('received');
    got.className = '';
    got.textContent = '';
    fillReceived(got, r);
    $('res-meta').textContent = '→ ' + r.to + ' · ' + r.bytes + '바이트 · ' + r.ms + 'ms';
  }

  function showError(message) {
    var got = $('received');
    got.className = '';
    got.textContent = '';
    got.appendChild(el('span', 'status-bad', '요청을 보내지 못했다: ' + message));
    $('res-meta').textContent = '';
  }

  function showWhy(text) {
    var why = $('why');
    why.hidden = !text;
    why.textContent = text || '';
  }

  /* ---------- 그림 ---------- */

  function setChip(id, text, cls) {
    var node = $(id);
    node.textContent = text;
    node.setAttribute('class', 'chip ' + (id === 'proxy-note' ? 'only-proxy ' : id === 'direct-note' ? 'only-direct ' : '') + (cls || ''));
  }

  function setDirect(port) {
    directPort = port;
    $('direct-sub').textContent = port === 8005 ? '포트 8005 · fork 없는 빌드' : '포트 ' + port + ' · 폴더 www';
    paintStatus();
  }

  function setStage(mode, port) {
    $('stage').setAttribute('class', 'mode-' + mode);
    $('stage').setAttribute('aria-label', mode === 'proxy' ? '클라이언트, 프록시, 백엔드 사이의 흐름' : '클라이언트와 웹 서버 사이의 흐름');
    if (mode === 'direct') {
      setDirect(port || 8004);
      /* 칸이 둘뿐이므로 그 둘이 크게 보이도록 그림의 보이는 범위를 좁힌다 */
      $('stage').setAttribute('viewBox', '0 105 840 185');
    } else {
      showExt(!$('node-ext').classList.contains('hidden'), $('ext-name').textContent);
    }
  }

  function resetStage(scene) {
    if (scene.shell) { return; }
    showExt(false);
    setStage(scene.stage, scene.port);
    clearMarks();
    setChip('client-chip', '', '');
    setChip('proxy-note', '', '');
    setChip('direct-note', '', '');
    $('dot').setAttribute('class', 'dot hidden');
    $('dot2').setAttribute('class', 'dot hidden');
    $('sent').className = 'empty';
    $('sent').textContent = '아직 보낸 요청이 없다';
    $('received').className = 'empty';
    $('received').textContent = '아직 받은 응답이 없다';
    $('res-meta').textContent = '';
  }

  function showExt(flag, name) {
    $('node-ext').classList.toggle('hidden', !flag);
    $('edge-ext').classList.toggle('hidden', !flag);
    $('stage').setAttribute('viewBox', flag ? '0 20 900 435' : '0 20 900 345');
    if (flag) { $('ext-name').textContent = name; }
  }

  function clearMarks() {
    Array.prototype.forEach.call(document.querySelectorAll('.node'), function (n) {
      n.classList.remove('hit', 'fail', 'busy');
    });
    Array.prototype.forEach.call(document.querySelectorAll('.edges line'), function (l) {
      l.classList.remove('active', 'res', 'err');
    });
  }

  function markEdge(id, cls) {
    var line = $(id);
    line.classList.remove('res', 'err');
    line.classList.add('active');
    if (cls) { line.classList.add(cls); }
  }

  function dotClass(dot, cls) { dot.setAttribute('class', 'dot ' + (cls || '')); }

  /* 점을 from에서 to까지 ms 동안 옮긴다. stopAt(0~1)이 있으면 그 비율에서 멈춘다 */
  function moveDot(dot, from, to, ms, stopAt) {
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

  function statusText(r) { return r.status_line.replace(/^HTTP\/\d\.\d /, ''); }

  /*
   * 클라이언트 ↔ 웹 서버(프록시 없음). 점은 요청을 보낼 때 떠나고, 실제 응답이 돌아온 뒤에 돌아온다.
   * pending: 응답을 기다리는 Promise. dy: 점의 세로 위치(두 요청을 함께 보일 때 겹치지 않게).
   */
  function travelDirect(dot, pending, dy) {
    var from = [POS.client[0] + 80, POS.client[1] + dy];
    var to = [POS.direct[0] - 105, POS.direct[1] + dy];
    dotClass(dot, '');
    markEdge('edge-direct');
    var out = moveDot(dot, from, to, 420).then(function () {
      $('node-direct').classList.add('busy');
      dotClass(dot, 'wait');
    });
    return Promise.all([out, pending]).then(function (both) {
      var r = both[1];
      var ok = r.ok && r.status >= 200 && r.status < 400;
      dotClass(dot, ok ? 'res' : 'err');
      markEdge('edge-direct', ok ? 'res' : 'err');
      return moveDot(dot, to, from, 420).then(function () {
        dotClass(dot, 'hidden');
        return r;
      });
    });
  }

  function sendDirect(port, raw, opt) {
    setStage('direct', port);
    clearMarks();
    setChip('client-chip', '', '');
    setChip('direct-note', '', '');
    return travelDirect($('dot'), call({ action: 'send', to: port, raw: raw }), 0).then(function (r) {
      $('node-direct').classList.remove('busy');
      if (!r.ok) {
        $('node-direct').classList.add('fail');
        setChip('client-chip', 'connect 실패', 'bad');
        throw new Error(r.error);
      }
      var ok = r.status >= 200 && r.status < 400;
      $('node-direct').classList.add(ok ? 'hit' : 'fail');
      setChip('client-chip', statusText(r), ok ? 'ok' : 'bad');
      showExchange(r);
      showWhy(opt.why);
      return pullLogs();
    });
  }

  function sendProxy(raw, opt) {
    setStage('proxy');
    var result;
    return call({ action: 'send', to: 8080, raw: raw }).then(function (r) {
      if (!r.ok) { throw new Error(r.error); }
      result = r;
      return pullLogs();
    }).then(function (fresh) {
      showExchange(result);
      showWhy(opt.why);
      return animateProxy(result, readDecision(fresh.proxy), opt.fast);
    });
  }

  function send(port, raw, opt) {
    opt = opt || {};
    var done = port === 8080 ? sendProxy(raw, opt) : sendDirect(port, raw, opt);
    return done.then(refreshStatus);
  }

  function sendMany(n) {
    var chain = Promise.resolve();
    for (var i = 0; i < n; i++) {
      chain = chain.then(function () { return send(8080, http('GET', '/api/'), { fast: true }); });
    }
    return chain;
  }

  /* 프록시를 거친 요청: 프록시 로그에 찍힌 목적지대로 점을 옮긴다 */
  function animateProxy(r, decision, fast) {
    var hop = fast ? 170 : 460;
    var pause = fast ? 60 : 240;
    var dot = $('dot');
    var ok = r.status >= 200 && r.status < 400;

    /* 프록시가 정한 목적지가 그림의 백엔드인지, 그림에 없는 다른 서버인지 */
    var target = null;
    if (decision && decision.port !== null) {
      var local = decision.host === '127.0.0.1' || decision.host === 'localhost';
      target = local && POS[decision.port] && decision.port !== 'direct' ? String(decision.port) : 'ext';
    }
    showExt(target === 'ext', decision ? decision.text : '');
    clearMarks();
    setChip('client-chip', '', '');
    setChip('proxy-note', '', '');

    dotClass(dot, '');
    dot.setAttribute('cx', POS.client[0]);
    dot.setAttribute('cy', POS.client[1]);

    var edgeId = target ? 'edge-' + target : null;
    var nodeId = target ? 'node-' + target : null;

    markEdge('edge-client');
    return moveDot(dot, POS.client, POS.proxy, hop).then(function () {
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
        return moveDot(dot, POS.proxy, POS[target], hop, 0.82).then(function () {
          $(nodeId).classList.add('fail');
          markEdge(edgeId, 'err');
          dotClass(dot, 'err');
          return sleep(pause);
        }).then(function () {
          var stopped = [Number(dot.getAttribute('cx')), Number(dot.getAttribute('cy'))];
          return moveDot(dot, stopped, POS.proxy, hop);
        });
      }
      return moveDot(dot, POS.proxy, POS[target], hop).then(function () {
        $(nodeId).classList.add('hit');
        if (counts[target] !== undefined) {
          counts[target] += 1;
          paintStatus();
        }
        dotClass(dot, ok ? 'res' : 'err');
        markEdge(edgeId, ok ? 'res' : 'err');
        return sleep(pause);
      }).then(function () {
        return moveDot(dot, POS[target], POS.proxy, hop);
      });
    }).then(function () {
      dotClass(dot, ok ? 'res' : 'err');
      markEdge('edge-client', ok ? 'res' : 'err');
      return moveDot(dot, POS.proxy, POS.client, hop);
    }).then(function () {
      $('node-proxy').classList.remove('busy');
      setChip('client-chip', statusText(r), ok ? 'ok' : 'bad');
      dotClass(dot, 'hidden');
    });
  }

  /* ---------- fd 표 (CGI 프로그램이 /proc 에서 읽어 온 값) ---------- */

  var STEP_NAME = {
    before: '① 연결 전',
    connected: '② 클라이언트가 connect한 직후 (요청은 아직 보내지 않음)',
    running: '② CGI 프로세스가 실행 중인 때',
    after: '③ 응답을 다 보내고 연결을 닫은 뒤'
  };
  var ROLE_NAME = { parent: '웹 서버 프로세스 (부모)', child: '자식 프로세스', cgi: 'CGI 프로세스' };

  function fdNote(role, f) {
    if (f.sock.indexOf('LISTEN') === 0) { return '듣기 소켓'; }
    if (f.sock.indexOf('ESTABLISHED') === 0) { return '연결 소켓'; }
    if (f.target.indexOf('pipe:') === 0) {
      if (role === 'cgi') { return f.fd === 0 ? '표준 입력: 요청 본문이 들어오는 파이프' : '표준 출력: 웹 서버가 읽는 파이프'; }
      return 'CGI 프로세스의 출력을 읽는 파이프';
    }
    return '';
  }

  function renderFds(r) {
    var box = $('fd-screen');
    if (!box) { return; }
    box.textContent = '';
    r.steps.forEach(function (step, si) {
      box.appendChild(el('div', 'step', STEP_NAME[step.name]));
      step.procs.forEach(function (p) {
        /* 첫 시점 뒤에는 0~2번 중 로그 파일, /dev/null을 가리키는 줄(부모와 같은 값)을 줄여 표가 한눈에 들어오게 한다 */
        var rows = p.fds.filter(function (f) { return si === 0 || f.fd > 2 || /^(pipe|socket):/.test(f.target); });
        var same = rows.length < p.fds.length ? '  (나머지 0~2번은 ①과 같다)' : '';
        box.appendChild(el('div', 'proc', ROLE_NAME[p.role] + ' ' + p.pid + same));
        rows.forEach(function (f) {
          var row = el('div');
          var target = f.target.replace(/^.*\/demo\/lab\/run\//, 'demo/lab/run/');
          row.appendChild(document.createTextNode('  ' + f.fd + ' → ' + target + ' '));
          if (f.sock) { row.appendChild(el('span', 'sockinfo', f.sock + ' ')); }
          var note = fdNote(p.role, f);
          if (note) { row.appendChild(el('span', f.sock ? 'note' : 'pipeinfo', '← ' + note)); }
          box.appendChild(row);
        });
      });
    });
    box.appendChild(el('div', 'note', '클라이언트(CGI 프로세스 lab) 쪽 소켓: fd ' + r.client_fd + ', 포트 ' + r.client_port));
  }

  function runFds(cgi) {
    var params = { action: 'fds', port: 8004 };
    if (cgi) { params.mode = 'cgi'; }
    setStage('direct', 8004);
    clearMarks();
    setChip('client-chip', '', '');
    setChip('direct-note', '', '');
    showWhy('');
    return travelDirect($('dot'), call(params), 0).then(function (r) {
      $('node-direct').classList.remove('busy');
      if (!r.ok) { throw new Error(r.error); }
      $('node-direct').classList.add('hit');
      setChip('client-chip', statusText(r), 'ok');
      setChip('direct-note', '프로세스 ' + r.pid, '');
      var sent = $('sent');
      sent.className = '';
      sent.textContent = '';
      sent.appendChild(el('span', 'first', r.sent.replace(/\r\n\r\n$/, '') + '\n'));
      var got = $('received');
      got.className = '';
      got.textContent = '';
      got.appendChild(el('span', 'status-ok', r.status_line + '\n'));
      got.appendChild(el('span', 'cut', '응답 전체 ' + r.bytes + '바이트'));
      $('res-meta').textContent = '→ ' + r.port;
      renderFds(r);
      return pullLogs();
    }).then(refreshStatus);
  }

  /* ---------- fork 있음과 없음: 느린 요청 뒤에 빠른 요청 ---------- */

  function runRace(port) {
    setStage('direct', port);
    clearMarks();
    setChip('client-chip', '', '');
    setChip('direct-note', '', '');
    showWhy('');
    var slowRaw = http('GET', '/cgi-bin/slow');
    var fastRaw = http('GET', '/index.html');
    var sent = $('sent');
    var got = $('received');
    sent.className = '';
    sent.textContent = '';
    got.className = '';
    got.textContent = '';
    $('res-meta').textContent = '→ ' + port;
    sent.appendChild(el('span', 'first', '느린 요청  GET /cgi-bin/slow HTTP/1.0\n'));

    function report(name, r) {
      var ok = r.ok && r.status === 200;
      got.appendChild(el('span', ok ? 'status-ok' : 'status-bad', name + '  ' + (r.ok ? r.status_line : r.error)));
      got.appendChild(el('span', 'len', r.ok ? '  ' + Math.round(r.ms) + 'ms\n' : '\n'));
      return r;
    }

    var slow = travelDirect($('dot'), call({ action: 'send', to: port, raw: slowRaw }), -12).then(function (r) { return report('느린 요청', r); });
    var fast = sleep(400).then(function () {
      sent.appendChild(el('span', 'first', '빠른 요청  GET /index.html HTTP/1.0  (0.4초 뒤에 보냄)\n'));
      return travelDirect($('dot2'), call({ action: 'send', to: port, raw: fastRaw }), 12);
    }).then(function (r) {
      report('빠른 요청', r);
      if (r.ok) { setChip('direct-note', '빠른 요청이 걸린 시간 ' + Math.round(r.ms) + 'ms', r.ms < 1000 ? 'ok' : 'bad'); }
      pullLogs().catch(function () {});
      return r;
    });
    return Promise.all([slow, fast]).then(function (both) {
      $('node-direct').classList.remove('busy');
      var f = both[1];
      if (f.ok) {
        showWhy(f.ms < 1000
          ? '빠른 요청은 ' + Math.round(f.ms) + 'ms 만에 응답을 받았다. 느린 요청은 자식 프로세스가 처리하고, 웹 서버 프로세스(부모)는 바로 accept로 돌아가 빠른 요청을 다른 자식 프로세스에게 맡겼다.'
          : '빠른 요청은 ' + Math.round(f.ms) + 'ms를 기다렸다. 웹 서버 프로세스가 느린 요청을 직접 처리하느라 accept로 돌아가지 못했고, 빠른 요청의 연결은 그동안 커널의 대기열에 있었다.');
      }
      return pullLogs();
    }).then(refreshStatus);
  }

  /* ---------- 시작 ---------- */

  buildCats();
  var saved = null;
  try { saved = localStorage.getItem('lab-scene'); } catch (e) { /* 저장된 것이 없으면 첫 장면부터 */ }
  showScene(SCENES[saved] ? saved : 'fd');
  refreshStatus();
  setInterval(function () { if (!busy) { refreshStatus(); pullLogs().catch(function () {}); } }, 2000);
})();
