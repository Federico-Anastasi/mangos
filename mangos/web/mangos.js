// mangos.js: the phone for the pages PIM makes. A page loads it with
//   <script src="/mangos.js"></script>
// and then has, besides the standard navigator.vibrate and devicemotion events (which the browser
// alone cannot give on this phone):
//   mangos.vibrate(ms)                  the motor, 1 to 2000 milliseconds
//   mangos.motion(fn, rate)             fn({x, y, z}) in m/s², gravity included, `rate` times a
//                                       second (up to 50); returns a function that stops it
//   mangos.compass(fn, rate)            fn({x, y, z}) in microtesla (not calibrated)
//   mangos.photo({front: true})         a promise of the address of a photo (3-4 seconds; the
//                                       person sees what the camera sees)
//   mangos.speak(text)                  PIM says the text aloud; the promise ends when it has said it
// vibrate, speak and photo take one request at a time: a call made while the previous one is still
// going is refused (its promise is rejected).
// The page runs isolated from the phone: every request goes to the UI that shows it, which hands
// it to the mangOS bridge.
(function () {
  'use strict';
  var waiting = {}, next = 1, readers = [], rate = 0;

  function ask(action, args) {
    return new Promise(function (resolve, reject) {
      var id = next++;
      waiting[id] = { resolve: resolve, reject: reject };
      window.parent.postMessage({ mangos: id, action: action, args: args || {} }, '*');
    });
  }

  window.addEventListener('message', function (e) {
    var m = e.data;
    if (!m || e.source !== window.parent) return;
    if (m.mangosReply !== undefined && waiting[m.mangosReply]) {
      var w = waiting[m.mangosReply];
      delete waiting[m.mangosReply];
      if (m.error) w.reject(new Error(m.error)); else w.resolve(m.result);
    } else if (m.mangosSensors) {
      readers.slice().forEach(function (r) {
        var v = m.mangosSensors[r.field];
        if (v) r.fn({ x: v[0], y: v[1], z: v[2] });
      });
    }
  });

  // The UI reads the sensors as fast as the most demanding reader wants, and stops with the last.
  function setRate() {
    var wanted = readers.reduce(function (most, r) { return Math.max(most, r.rate); }, 0);
    if (wanted !== rate) { rate = wanted; ask('sensors', { rate: rate }); }
  }
  function read(field, fn, wantedRate) {
    var reader = { field: field, fn: fn, rate: Math.max(1, Math.min(50, wantedRate || 30)) };
    readers.push(reader);
    setRate();
    return function stop() {
      readers = readers.filter(function (r) { return r !== reader; });
      setRate();
    };
  }

  var mangos = window.mangos = {
    vibrate: function (ms) { return ask('vibrate', { ms: ms || 200 }); },
    motion: function (fn, wantedRate) { return read('acceleration', fn, wantedRate); },
    compass: function (fn, wantedRate) { return read('magnetic_field', fn, wantedRate); },
    photo: function (options) { return ask('photo', { front: !!(options && options.front) }); },
    speak: function (text) { return ask('speak', { text: String(text) }); }
  };

  // The standard ways, for pages written as for any phone.
  navigator.vibrate = function (pattern) {
    var ms = Array.isArray(pattern)
      ? pattern.reduce(function (sum, v, i) { return i % 2 ? sum : sum + v; }, 0)
      : pattern;
    if (ms > 0) mangos.vibrate(ms).catch(function () {});     // the standard call never fails
    return true;
  };
  var motionListeners = 0, stopMotion = null, last = null;
  var addListener = window.addEventListener, removeListener = window.removeEventListener;
  window.addEventListener = function (type, fn, options) {
    if (type === 'devicemotion' && motionListeners++ === 0) {
      stopMotion = mangos.motion(function (a) {
        var now = performance.now();
        var event = new Event('devicemotion');
        event.accelerationIncludingGravity = a;
        event.acceleration = null;
        event.rotationRate = null;
        event.interval = last ? now - last : 20;
        last = now;
        window.dispatchEvent(event);
      }, 50);
    }
    return addListener.call(window, type, fn, options);
  };
  window.removeEventListener = function (type, fn, options) {
    if (type === 'devicemotion' && motionListeners > 0 && --motionListeners === 0 && stopMotion) {
      stopMotion();
      stopMotion = null;
    }
    return removeListener.call(window, type, fn, options);
  };
})();
