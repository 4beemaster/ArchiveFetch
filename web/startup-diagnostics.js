(function () {
  'use strict';
  var token = '', pending = [], sent = 0, checking = false;
  var names = ['Error', 'SyntaxError', 'TypeError', 'ReferenceError', 'RangeError', 'URIError', 'EvalError', 'AbortError', 'TimeoutError'];
  function source(value) {
    var anchor = document.createElement('a');
    anchor.href = value || '';
    var filename = anchor.pathname.split('/').pop();
    return ['app.js', 'app.css', 'startup-diagnostics.js'].indexOf(filename) >= 0 ? filename : 'other';
  }
  function flush() {
    if (!token || !pending.length) return;
    while (pending.length) {
      var request = new XMLHttpRequest();
      request.open('POST', '/api/client/error', true);
      request.timeout = 10000;
      request.setRequestHeader('Content-Type', 'application/json');
      request.setRequestHeader('X-Archive-Session', token);
      request.send(JSON.stringify(pending.shift()));
    }
  }
  function connect(value) { token = value; flush(); }
  function report(kind, error, filename, line, column, stage) {
    if (sent >= 16) return;
    sent += 1;
    var name = error && error.name;
    pending.push({ kind: kind, error: names.indexOf(name) >= 0 ? name : 'Error', source: source(filename),
      line: Number(line) || 0, column: Number(column) || 0, stage: stage || 'runtime' });
    if (token) { flush(); return; }
    if (checking) return;
    checking = true;
    var request = new XMLHttpRequest();
    request.open('GET', '/session', true);
    request.timeout = 10000;
    request.onload = function () {
      if (request.status !== 200) return;
      try {
        var session = JSON.parse(request.responseText);
        if (session.logging && session.token) connect(session.token);
      } catch (ignored) {}
    };
    request.send();
  }
  window.ArchiveFetchDiagnostics = {
    connect: connect,
    failure: function (stage, error) { report('bootstrap_failure', error, 'app.js', 0, 0, stage); }
  };
  window.addEventListener('error', function (event) {
    var target = event.target;
    if (target && target !== window) report('resource_error', null, target.src || target.href, 0, 0);
    else report('script_error', event.error, event.filename, event.lineno, event.colno);
  }, true);
  window.addEventListener('unhandledrejection', function (event) {
    report('unhandled_rejection', event.reason, 'app.js', 0, 0);
  });
}());