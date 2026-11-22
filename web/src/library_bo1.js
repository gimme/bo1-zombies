// JavaScript side of the WebAssembly build's platform layer (linked with --js-library).
// Everything here runs on the page's own thread: input listeners and gamepad polling. The engine's threads read
// what they write through shared memory (web_platform.h: Bo1InputShared).

addToLibrary({
  // engine -> page: 'status' / 'error' / 'exit' / 'build' / 'ended' text for the overlay, and 'game' for what the game
  // says to its lobby (zombies-hub's web/public/player.js: Module.bo1Status)
  bo1_status__proxy: 'sync',
  bo1_status: (what, text) => {
    const f = Module['bo1Status'];
    if (f) f(UTF8ToString(what), UTF8ToString(text));
  },

  // What the engine hangs on the page, which bo1Stop takes down: the listeners it adds (on bo1Life.abort's signal), its
  // workers of its own (not its threads: Emscripten's), and its audio contexts
  $bo1Life: '{ abort: new AbortController(), workers: [], audio: [] }',

  // The page ends the engine (zombies-hub's web/public/player.js: stop), once its game ended, and stays, to start
  // another (bo1Engine: web/CMakeLists.txt) as it did before the game, with no reload: what the engine hung on the page
  // goes as a reload would take it, so that the browser can free it all, the engine's memory with the rest. Its threads
  // (and this one's wait on its mailbox, which would keep the memory: Emscripten's own exit takes them down so), its
  // listeners, workers and sound; the replays of its frames on this thread's animation frames (Bo1_StopReplay), and its
  // GPU device; the mouse and keyboard it held. It returns the engine's memory, which the next may take
  // (Module.wasmMemory) while the browser has yet to free it.
  $bo1Stop__deps: ['$bo1Life', '$bo1GpuStop', 'Bo1_StopReplay'],
  $bo1Stop__postset: "Module['bo1Stop'] = bo1Stop;",
  $bo1Stop: () => {
    _Bo1_StopReplay();
    PThread.terminateRuntime();
    ABORT = true;
    bo1Life.abort.abort();
    for (const w of bo1Life.workers) w.terminate();
    for (const ctx of bo1Life.audio) ctx.close().catch(() => {});
    bo1Life.workers = [];
    bo1Life.audio = [];
    bo1GpuStop();
    if (document.pointerLockElement) document.exitPointerLock();
    if (navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
    return wasmMemory;
  },

  // sound output: an AudioWorklet playing the ring the XAudio2 mixer fills (xaudio2_web.cpp: Bo1AudioRing). Browsers
  // start audio contexts suspended until the player's first gesture on the page: a click, a key, or a gamepad's press
  // (bo1-padpress: pollPads, zombies-hub's web/src/gestures.ts: GESTURES); the mixer keeps time meanwhile.
  bo1_audio_start__proxy: 'sync',
  bo1_audio_start__deps: ['$bo1Life'],
  bo1_audio_start: (ring, capacity, channels, rate) => {
    if (typeof AudioContext === 'undefined' || typeof AudioWorkletNode === 'undefined') return 0;
    const processor = `
      class Bo1Audio extends AudioWorkletProcessor {
        constructor(options) {
          super();
          const o = options.processorOptions;
          this.i32 = new Int32Array(o.buffer);
          this.f32 = new Float32Array(o.buffer);
          this.head = o.ring >> 2;            // writePos, readPos, ticks
          this.data = (o.ring + 32) >> 2;     // Bo1AudioRing.data
          this.capacity = o.capacity;
          this.channels = o.channels;
        }
        process(inputs, outputs) {
          const out = outputs[0];
          const n = out[0].length;
          const i32 = this.i32, f32 = this.f32, head = this.head, ch = this.channels;
          Atomics.add(i32, head + 2, 1);
          const w = Atomics.load(i32, head);
          const r = Atomics.load(i32, head + 1);
          if (((w - r) | 0) < n) {
            Atomics.add(i32, head + 5, 1); // starved (Bo1AudioRing.underruns): silence
            return true;
          }
          for (let i = 0; i < n; i++) {
            const at = this.data + (((r + i) >>> 0) % this.capacity) * ch;
            for (let c = 0; c < out.length; c++) out[c][i] = f32[at + (c < ch ? c : ch - 1)];
          }
          Atomics.store(i32, head + 1, (r + n) | 0);
          return true;
        }
      }
      registerProcessor('bo1-audio', Bo1Audio);`;
    let ctx;
    try {
      ctx = new AudioContext({ sampleRate: rate, latencyHint: 'interactive' });
    } catch (e) {
      console.warn('sound: no AudioContext', e);
      return 0;
    }
    bo1Life.audio.push(ctx);
    Module['bo1AudioContext'] = ctx;
    Module['bo1AudioRing'] = ring;
    const url = URL.createObjectURL(new Blob([processor], { type: 'text/javascript' }));
    ctx.audioWorklet.addModule(url).then(() => {
      const node = new AudioWorkletNode(ctx, 'bo1-audio', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels],
        processorOptions: { buffer: HEAPU8.buffer, ring, capacity, channels },
      });
      node.connect(ctx.destination);
    }).catch((e) => console.warn('sound: the audio worklet failed', e));
    const resume = () => { if (ctx.state !== 'running') ctx.resume().catch(() => {}); };
    for (const type of ['pointerdown', 'keydown', 'touchstart', 'bo1-padpress']) {
      window.addEventListener(type, resume, { capture: true, signal: bo1Life.abort.signal });
    }
    resume();
    return 1;
  },

  // The map's loading movie (bink_web.cpp: Bo1Movie), which a thread of the engine's decodes ahead into the shared
  // memory from the engine's start: shown on a canvas of its own under the game's (zombies-hub's web/public/player.js:
  // #movie), by a worker on its own animation frames, and its sound played by an AudioWorklet, so that nothing the
  // engine does as it starts and loads holds either up. The clock is the sound's while the worklet plays it (its place
  // in it, less the output's latency), else the page's: a page that may not play sound yet still shows the movie, and
  // its sound comes in where the pictures are once it may. A picture is shown once its time has come, the latest the
  // thread has; one the clock is already past is left out (the thread doesn't put it in the ring). The worker says when
  // the movie shows (its first picture) and when it is over (its last picture's time has passed, or the engine stopped
  // it), to the page (Module.bo1Status: movie), which shows its canvas and the game's over it, and then the game's
  // alone.
  bo1_movie_show__proxy: 'sync',
  bo1_movie_show__deps: ['$bo1Life'],
  bo1_movie_show: (movie) => {
    const names = ['width', 'height', 'frames', 'rate', 'rateDiv', 'slots', 'slotBytes', 'pictures', 'soundRate',
      'soundFrames', 'sound', 'decoded', 'soundIn', 'ended', 'shown', 'due', 'showing', 'over', 'soundAt', 'soundTicks',
      'soundSeek', 'stop'];
    const F = {};
    names.forEach((name, i) => (F[name] = i));
    const fields = JSON.stringify(F);
    const base = movie >>> 2;
    const status = (what) => {
      const f = Module['bo1Status'];
      if (f) f('movie', what);
    };
    const canvas = document.getElementById('movie');
    if (!canvas || !canvas.transferControlToOffscreen || typeof VideoFrame === 'undefined') {
      // the engine's loading screen shows the map's still instead
      console.warn('movie: this browser can\'t show the loading movie (it has no OffscreenCanvas or VideoFrame)');
      Atomics.store(HEAP32, base + F.over, 1);
      return;
    }
    const presenter = `
      'use strict';
      const F = ${fields};
      let i32, u8, base, latency = 0;
      const get = (k) => Atomics.load(i32, base + F[k]);
      const set = (k, v) => Atomics.store(i32, base + F[k], v);
      onmessage = (e) => {
        if (e.data.latency !== undefined) {
          latency = e.data.latency;
          return;
        }
        i32 = new Int32Array(e.data.buffer);
        u8 = new Uint8Array(e.data.buffer);
        base = e.data.movie >>> 2;
        play(e.data.canvas);
      };
      function play(canvas) {
        const ctx = canvas.getContext('2d', { alpha: false });
        const w = get('width'), h = get('height'), frames = get('frames'), fps = get('rate') / get('rateDiv');
        const slots = get('slots'), slotBytes = get('slotBytes'), pictures = get('pictures') >>> 0;
        const soundRate = get('soundRate');
        const cw = (w + 1) >> 1, ch = (h + 1) >> 1;
        const layout = [{ offset: 0, stride: w }, { offset: w * h, stride: cw }, { offset: w * h + cw * ch, stride: cw }];
        // Bink's YCbCr: BT.601's, in video range
        const colorSpace = { primaries: 'smpte170m', transfer: 'smpte170m', matrix: 'smpte170m', fullRange: false };
        const copy = new Uint8Array(w * h + cw * ch * 2);
        // the picture in its proportions, as large as the canvas takes it, as the engine draws a movie (cl_cin.cpp)
        let dw = canvas.width, dh = canvas.height;
        if (dw * h > dh * w) dw = Math.round(dh * w / h);
        else dh = Math.round(dw * h / w);
        const dx = (canvas.width - dw) >> 1, dy = (canvas.height - dh) >> 1;
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        let t = 0, last = 0, shown = 0, started = false, done = false;
        let ticks = get('soundTicks'), ticksAt = -1e9, sound = 0; // the clock: 0 the page's, 1 the sound's to be, 2 the sound's
        // how it went: the pictures shown, and the longest any stayed up (a 30 a second movie's are up 33 ms)
        let count = 0, longest = 0, shownAt = 0;
        function over() {
          done = true;
          ctx.fillRect(0, 0, canvas.width, canvas.height);
          set('showing', 0);
          set('over', 1);
          Atomics.notify(i32, base + F.shown);
          postMessage({ what: 'over', shown: count, frames, longest: Math.round(longest), stopped: !!get('stop') });
        }
        // the movie at the time now: false once it is over
        function frame(now) {
          if (get('stop')) return over();
          const decoded = get('decoded');
          if (!started) {
            if (decoded < 1) return get('ended') ? over() : true;
            started = true;
            last = now;
          }
          let at = t + Math.max(0, now - last) / 1000;
          last = now;
          if (soundRate) {
            const playing = get('soundTicks');
            if (playing !== ticks) {
              ticks = playing;
              ticksAt = now;
            }
            if (now - ticksAt > 200) {
              sound = 0;
            } else if (sound === 0) {
              // the worklet plays: it goes to where the pictures are, and its place is the clock from then on
              set('soundSeek', Math.round(at * soundRate));
              sound = 1;
            } else if (sound === 1 && get('soundSeek') < 0) {
              sound = 2;
              console.log("page: the loading movie's sound plays, from " + at.toFixed(1) + ' s in');
            }
            if (sound === 2) at = Math.max(t, get('soundAt') / soundRate - latency);
          }
          t = at;
          const due = Math.floor(t * fps) + 1;
          set('due', due);
          const k = Math.min(due, decoded, frames);
          if (k > shown) {
            const from = pictures + (k % slots) * slotBytes;
            copy.set(u8.subarray(from, from + copy.length));
            const picture = new VideoFrame(copy, { format: 'I420', codedWidth: w, codedHeight: h, layout, colorSpace,
              timestamp: Math.round((k - 1) * 1e6 / fps) });
            ctx.drawImage(picture, dx, dy, dw, dh);
            picture.close();
            shown = k;
            set('shown', k);
            Atomics.notify(i32, base + F.shown);
            if (shownAt) longest = Math.max(longest, now - shownAt);
            shownAt = now;
            count++;
            if (!get('showing')) {
              set('showing', 1);
              postMessage({ what: 'shows' });
            }
          }
          if (due > frames || (get('ended') && due > get('decoded'))) return over();
          return true;
        }
        // On the animation frames; and on a timer while there are none (a hidden page has none), so that the movie still
        // ends in its time, and the loaded game that waits for it goes in (the engine's cinematics went on so too).
        let animated = 0;
        const animate = (now) => {
          animated = performance.now();
          if (!done && frame(now)) requestAnimationFrame(animate);
        };
        requestAnimationFrame(animate);
        const timer = setInterval(() => {
          if (done) clearInterval(timer);
          else if (performance.now() - animated > 250) frame(performance.now());
        }, 100);
      }`;
    const worklet = `
      const F = ${fields};
      class Bo1Movie extends AudioWorkletProcessor {
        constructor(options) {
          super();
          const o = options.processorOptions;
          this.i32 = new Int32Array(o.buffer);
          this.f32 = new Float32Array(o.buffer);
          this.base = o.movie >>> 2;
          this.ring = (Atomics.load(this.i32, this.base + F.sound) >>> 0) >>> 2;
          this.capacity = Atomics.load(this.i32, this.base + F.soundFrames);
          this.at = -1; // its place in the sound: none until the page's clock gives it one (soundSeek)
        }
        process(inputs, outputs) {
          const i32 = this.i32, base = this.base;
          if (Atomics.load(i32, base + F.stop) || Atomics.load(i32, base + F.over)) return false;
          Atomics.add(i32, base + F.soundTicks, 1);
          const seek = Atomics.exchange(i32, base + F.soundSeek, -1);
          if (seek >= 0) this.at = seek;
          if (this.at < 0) return true;
          const out = outputs[0], n = out[0].length, f32 = this.f32, capacity = this.capacity;
          const decoded = Atomics.load(i32, base + F.soundIn);
          for (let i = 0; i < n; i++) {
            const p = this.at + i;
            let l = 0, r = 0;
            if (p < decoded && p >= decoded - capacity) {
              const at = this.ring + (p % capacity) * 2;
              l = f32[at];
              r = f32[at + 1];
            }
            out[0][i] = l;
            if (out.length > 1) out[1][i] = r;
          }
          this.at += n;
          Atomics.store(i32, base + F.soundAt, this.at);
          return true;
        }
      }
      registerProcessor('bo1-movie', Bo1Movie);`;
    const game = Module['canvas'];
    canvas.width = game.width;
    canvas.height = game.height;
    const offscreen = canvas.transferControlToOffscreen();
    const worker = new Worker(URL.createObjectURL(new Blob([presenter], { type: 'text/javascript' })), { name: 'bo1-movie' });
    bo1Life.workers.push(worker);
    let ctx = null;
    const done = () => {
      worker.terminate();
      if (ctx) ctx.close().catch(() => {});
      ctx = null;
    };
    worker.onmessage = (e) => {
      const d = e.data;
      if (d.what === 'over') {
        done();
        console.log('page: the loading movie ' + (d.stopped ? 'was stopped' : 'ended') + ': ' + d.shown + ' of its ' +
          d.frames + ' pictures shown, none up longer than ' + d.longest + ' ms');
      }
      status(d.what);
    };
    worker.onerror = (e) => {
      console.warn('movie: ' + e.message);
      Atomics.store(HEAP32, base + F.showing, 0);
      Atomics.store(HEAP32, base + F.over, 1);
      done();
      status('over');
    };
    worker.postMessage({ buffer: HEAPU8.buffer, movie, canvas: offscreen }, [offscreen]);
    status('starts');
    const rate = HEAP32[base + F.soundRate];
    if (!rate || typeof AudioContext === 'undefined' || typeof AudioWorkletNode === 'undefined') return;
    try {
      ctx = new AudioContext({ sampleRate: rate, latencyHint: 'interactive' });
    } catch (e) {
      console.warn('movie: no sound (' + e.message + ')');
      return;
    }
    const sound = ctx;
    bo1Life.audio.push(sound);
    sound.audioWorklet.addModule(URL.createObjectURL(new Blob([worklet], { type: 'text/javascript' }))).then(() => {
      if (ctx !== sound) return;
      const node = new AudioWorkletNode(sound, 'bo1-movie', { numberOfInputs: 0, numberOfOutputs: 1,
        outputChannelCount: [2], processorOptions: { buffer: HEAPU8.buffer, movie } });
      node.connect(sound.destination);
      worker.postMessage({ latency: (sound.baseLatency || 0) + (sound.outputLatency || 0) });
    }).catch((e) => console.warn('movie: the audio worklet failed', e));
    // as the game's sound (bo1_audio_start): a page with no gesture yet may play none until it has one
    const resume = () => {
      if (sound.state === 'suspended') sound.resume().catch(() => {});
    };
    for (const type of ['pointerdown', 'keydown', 'touchstart', 'bo1-padpress']) {
      window.addEventListener(type, resume, { capture: true, signal: bo1Life.abort.signal });
    }
    resume();
  },

  // the network: UDP datagrams over WebTransport (web_net.cpp: Bo1NetShared). The sessions live on a worker of their
  // own, so datagrams come and go while this thread renders: it waits on the engine's outgoing ring and fills the
  // incoming one. Each server (host:port) gets one session, opened by the first datagram to it, at the path the page
  // gives (Module.bo1SessionPath: /play/<token>, the player's seat in their room's game, zombies-hub's
  // server/relay.go). Its self-signed certificate is accepted by the hash that the server that served the page gives
  // (/certhash, zombies-hub's server/web.go): the web client plays on that server. Without one, the certificate has to
  // be one browsers trust.
  bo1_net_start__proxy: 'sync',
  bo1_net_start__deps: ['$bo1Life'],
  bo1_net_start: (shared, fromPageOffset, slotsOffset, slotSize, hostOffset, hostSize, dataOffset, slots, maxData) => {
    const code = `
      'use strict';
      let i32, u8, cfg;
      const sessions = new Map();
      const decoder = new TextDecoder();
      onmessage = (e) => {
        cfg = e.data;
        i32 = new Int32Array(cfg.buffer);
        u8 = new Uint8Array(cfg.buffer);
        pump(cfg.shared, cfg.shared + cfg.fromPageOffset);
      };
      const slotAt = (ring, index) => ring + cfg.slotsOffset + ((index >>> 0) % cfg.slots) * cfg.slotSize;

      // engine -> sessions
      async function pump(out, into) {
        const HEAD = out >> 2, TAIL = HEAD + 1;
        for (;;) {
          const head = Atomics.load(i32, HEAD);
          let tail = Atomics.load(i32, TAIL);
          for (; tail !== head; tail = (tail + 1) | 0) {
            const slot = slotAt(out, tail);
            const length = i32[(slot + 8) >> 2];
            if (length < 0) {
              closeAll();
            } else {
              let end = slot + cfg.hostOffset;
              while (end < slot + cfg.hostOffset + cfg.hostSize && u8[end]) end++;
              const host = decoder.decode(u8.slice(slot + cfg.hostOffset, end));
              send(host, i32[(slot + 4) >> 2], i32[slot >> 2], u8.slice(slot + cfg.dataOffset, slot + cfg.dataOffset + length), into);
            }
            Atomics.store(i32, TAIL, (tail + 1) | 0);
          }
          const wait = Atomics.waitAsync(i32, HEAD, head);
          if (wait.async) await wait.value;
        }
      }

      function send(host, port, ip, datagram, into) {
        const key = host + ':' + port;
        let s = sessions.get(key);
        if (!s) {
          s = { key, host, port, ip, queue: [], writer: null, ended: false };
          sessions.set(key, s);
          open(s, into);
        }
        if (s.writer) s.writer.write(datagram).catch(() => {});
        else if (!s.ended && s.queue.length < 64) s.queue.push(datagram);
      }

      async function open(s, into) {
        const authority = (s.host.includes(':') ? '[' + s.host + ']' : s.host) + ':' + s.port;
        try {
          let hash = '';
          try {
            const r = await fetch(cfg.origin + '/certhash', { cache: 'no-store' });
            if (r.ok) hash = (await r.text()).trim();
          } catch (e) {
            // no hash: a certificate browsers trust, or no server
          }
          const options = { requireUnreliable: true, congestionControl: 'low-latency' };
          if (/^[0-9a-f]{64}$/i.test(hash)) {
            const value = new Uint8Array(hash.match(/../g).map((h) => parseInt(h, 16)));
            options.serverCertificateHashes = [{ algorithm: 'sha-256', value }];
          }
          const wt = new WebTransport('https://' + authority + cfg.path, options);
          s.wt = wt;
          wt.closed.then(() => end(s, 'the server closed the connection'), (e) => end(s, (e && e.message) || String(e)));
          await wt.ready;
          if (s.ended) return;
          s.writer = wt.datagrams.writable.getWriter();
          for (const d of s.queue) s.writer.write(d).catch(() => {});
          s.queue = [];
          postMessage({ log: 'WebTransport to ' + s.key + ' open' });
          if (wt.datagrams.maxDatagramSize < cfg.maxData)
            postMessage({ log: s.key + ' takes datagrams of ' + wt.datagrams.maxDatagramSize + ' bytes only: larger ones are lost' });
          const reader = wt.datagrams.readable.getReader();
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            receive(s, value, into);
          }
        } catch (e) {
          end(s, 'cannot connect (' + ((e && e.message) || e) + ')');
        }
      }

      // sessions -> engine; a full ring loses the datagram, as a full socket buffer would
      function receive(s, datagram, into) {
        const HEAD = into >> 2, TAIL = HEAD + 1, DROPPED = HEAD + 2;
        const head = Atomics.load(i32, HEAD);
        if (((head - Atomics.load(i32, TAIL)) >>> 0) >= cfg.slots || datagram.length > cfg.maxData) {
          Atomics.add(i32, DROPPED, 1);
          return;
        }
        const slot = slotAt(into, head);
        i32[slot >> 2] = s.ip;
        i32[(slot + 4) >> 2] = s.port;
        i32[(slot + 8) >> 2] = datagram.length;
        u8.set(datagram, slot + cfg.dataOffset);
        Atomics.store(i32, HEAD, (head + 1) | 0);
      }

      // a session that ended drops what is sent to it for a moment; then the next datagram opens a new one (the
      // engine resends while it connects)
      function end(s, why) {
        if (s.ended) return;
        s.ended = true;
        s.writer = null;
        s.queue = [];
        try { if (s.wt) s.wt.close(); } catch (e) {}
        if (!s.quiet) postMessage({ log: s.key + ': ' + why, status: 'Server ' + s.key + ': ' + why });
        setTimeout(() => { if (sessions.get(s.key) === s) sessions.delete(s.key); }, 2000);
      }

      // the engine closed its socket
      function closeAll() {
        for (const s of sessions.values()) {
          s.quiet = true;
          end(s, 'closed');
        }
        sessions.clear();
      }`;
    if (typeof WebTransport === 'undefined') {
      console.warn('net: this browser has no WebTransport; only the engine\'s own local server works');
      return;
    }
    const worker = new Worker(URL.createObjectURL(new Blob([code], { type: 'text/javascript' })), { name: 'bo1-net' });
    bo1Life.workers.push(worker);
    worker.onmessage = (e) => {
      if (e.data.log) console.log('net: ' + e.data.log);
      const f = Module['bo1Status'];
      if (e.data.status && f) f('status', e.data.status);
    };
    worker.onerror = (e) => console.warn('net: the network worker failed', e.message);
    worker.postMessage({
      buffer: HEAPU8.buffer, shared, fromPageOffset, slotsOffset, slotSize, hostOffset, hostSize, dataOffset, slots, maxData,
      origin: location.origin, // the page's: a worker's own is its blob: URL
      path: Module['bo1SessionPath'] || '/',
    });
  },

  // KeyboardEvent.code -> [virtual key, set-1 scan code, extended]: what a US keyboard sends to MainWndProc
  $bo1KeyTable: {
    Escape: [0x1B, 0x01, 0], Digit1: [0x31, 0x02, 0], Digit2: [0x32, 0x03, 0], Digit3: [0x33, 0x04, 0],
    Digit4: [0x34, 0x05, 0], Digit5: [0x35, 0x06, 0], Digit6: [0x36, 0x07, 0], Digit7: [0x37, 0x08, 0],
    Digit8: [0x38, 0x09, 0], Digit9: [0x39, 0x0A, 0], Digit0: [0x30, 0x0B, 0], Minus: [0xBD, 0x0C, 0],
    Equal: [0xBB, 0x0D, 0], Backspace: [0x08, 0x0E, 0], Tab: [0x09, 0x0F, 0], KeyQ: [0x51, 0x10, 0],
    KeyW: [0x57, 0x11, 0], KeyE: [0x45, 0x12, 0], KeyR: [0x52, 0x13, 0], KeyT: [0x54, 0x14, 0],
    KeyY: [0x59, 0x15, 0], KeyU: [0x55, 0x16, 0], KeyI: [0x49, 0x17, 0], KeyO: [0x4F, 0x18, 0],
    KeyP: [0x50, 0x19, 0], BracketLeft: [0xDB, 0x1A, 0], BracketRight: [0xDD, 0x1B, 0], Enter: [0x0D, 0x1C, 0],
    ControlLeft: [0x11, 0x1D, 0], KeyA: [0x41, 0x1E, 0], KeyS: [0x53, 0x1F, 0], KeyD: [0x44, 0x20, 0],
    KeyF: [0x46, 0x21, 0], KeyG: [0x47, 0x22, 0], KeyH: [0x48, 0x23, 0], KeyJ: [0x4A, 0x24, 0],
    KeyK: [0x4B, 0x25, 0], KeyL: [0x4C, 0x26, 0], Semicolon: [0xBA, 0x27, 0], Quote: [0xDE, 0x28, 0],
    Backquote: [0xC0, 0x29, 0], ShiftLeft: [0x10, 0x2A, 0], Backslash: [0xDC, 0x2B, 0], KeyZ: [0x5A, 0x2C, 0],
    KeyX: [0x58, 0x2D, 0], KeyC: [0x43, 0x2E, 0], KeyV: [0x56, 0x2F, 0], KeyB: [0x42, 0x30, 0],
    KeyN: [0x4E, 0x31, 0], KeyM: [0x4D, 0x32, 0], Comma: [0xBC, 0x33, 0], Period: [0xBE, 0x34, 0],
    Slash: [0xBF, 0x35, 0], ShiftRight: [0x10, 0x36, 0], NumpadMultiply: [0x6A, 0x37, 0], AltLeft: [0x12, 0x38, 0],
    Space: [0x20, 0x39, 0], CapsLock: [0x14, 0x3A, 0], F1: [0x70, 0x3B, 0], F2: [0x71, 0x3C, 0],
    F3: [0x72, 0x3D, 0], F4: [0x73, 0x3E, 0], F5: [0x74, 0x3F, 0], F6: [0x75, 0x40, 0], F7: [0x76, 0x41, 0],
    F8: [0x77, 0x42, 0], F9: [0x78, 0x43, 0], F10: [0x79, 0x44, 0], NumLock: [0x90, 0x45, 0],
    ScrollLock: [0x91, 0x46, 0], Numpad7: [0x67, 0x47, 0], Numpad8: [0x68, 0x48, 0], Numpad9: [0x69, 0x49, 0],
    NumpadSubtract: [0x6D, 0x4A, 0], Numpad4: [0x64, 0x4B, 0], Numpad5: [0x65, 0x4C, 0], Numpad6: [0x66, 0x4D, 0],
    NumpadAdd: [0x6B, 0x4E, 0], Numpad1: [0x61, 0x4F, 0], Numpad2: [0x62, 0x50, 0], Numpad3: [0x63, 0x51, 0],
    Numpad0: [0x60, 0x52, 0], NumpadDecimal: [0x6E, 0x53, 0], IntlBackslash: [0xE2, 0x56, 0], F11: [0x7A, 0x57, 0],
    F12: [0x7B, 0x58, 0], NumpadEnter: [0x0D, 0x1C, 1], ControlRight: [0x11, 0x1D, 1], NumpadDivide: [0x6F, 0x35, 1],
    PrintScreen: [0x2C, 0x37, 1], AltRight: [0x12, 0x38, 1], Pause: [0x13, 0x45, 0], Home: [0x24, 0x47, 1],
    ArrowUp: [0x26, 0x48, 1], PageUp: [0x21, 0x49, 1], ArrowLeft: [0x25, 0x4B, 1], ArrowRight: [0x27, 0x4D, 1],
    End: [0x23, 0x4F, 1], ArrowDown: [0x28, 0x50, 1], PageDown: [0x22, 0x51, 1], Insert: [0x2D, 0x52, 1],
    Delete: [0x2E, 0x53, 1], MetaLeft: [0x5B, 0x5B, 1], MetaRight: [0x5C, 0x5C, 1], ContextMenu: [0x5D, 0x5D, 1],
  },

  bo1_input_install__proxy: 'sync',
  bo1_input_install__deps: ['$bo1KeyTable', '$bo1Life'],
  bo1_input_install: (shared, eventsOffset, eventStride, cursorOffset, keyStateOffset, sizeOffset, gamepadOffset, inputTimeOffset) => {
    const canvas = Module['canvas'] || document.getElementById('canvas');
    const signal = bo1Life.abort.signal;
    const on = (target, type, f) => target.addEventListener(type, f, { signal });
    const HEAD = shared >> 2;
    const TAIL = (shared + 4) >> 2;
    const QUEUE = 1024;
    const CURSOR_X = (shared + cursorOffset) >> 2;
    const CURSOR_Y = CURSOR_X + 1;
    const LOCKED = CURSOR_X + 2;
    const FOCUS = CURSOR_X + 3;
    const MOUSE_LOOK = CURSOR_X + 4; // the engine's (web_platform.h)
    const CURSOR_SHOWN = CURSOR_X + 5;
    const MOUSE_IDLE = CURSOR_X + 6;
    // the player's Full Screen setting: as the engine has it, and what the page sets it to, -1 once the engine has
    // taken that (web_platform.h: fullScreen, fullScreenSet)
    const FULL_SCREEN = CURSOR_X + 7;
    const FULL_SCREEN_SET = CURSOR_X + 8;
    const KEYS = (shared + keyStateOffset) >> 2;
    const WIDTH = (shared + sizeOffset) >> 2;
    const HEIGHT = WIDTH + 1;
    const INPUT_TIME = (shared + inputTimeOffset) >> 3;

    // the back buffer size: the page sized the canvas (zombies-hub's web/public/player.js: start)
    const w = canvas.width, h = canvas.height;
    Atomics.store(HEAP32, WIDTH, w);
    Atomics.store(HEAP32, HEIGHT, h);
    Atomics.store(HEAP32, CURSOR_X, w >> 1);
    Atomics.store(HEAP32, CURSOR_Y, h >> 1);
    Atomics.store(HEAP32, FOCUS, document.hasFocus() ? 1 : 0);
    // as the player's config has it, which the page read before the game started (zombies-hub's web/public/player.js:
    // start), until the engine has read it too
    Atomics.store(HEAP32, FULL_SCREEN, Module['bo1FullScreen'] === false ? 0 : 1);

    const push = (type, a, b, c) => {
      const head = Atomics.load(HEAP32, HEAD);
      const tail = Atomics.load(HEAP32, TAIL);
      if (head - tail >= QUEUE) return; // the engine is not pumping (loading): drop
      const base = shared + eventsOffset + (head % QUEUE) * eventStride;
      HEAP32[base >> 2] = type;
      HEAP32[(base >> 2) + 1] = a;
      HEAP32[(base >> 2) + 2] = b;
      HEAP32[(base >> 2) + 3] = c;
      HEAPF64[(base + 16) >> 3] = performance.now();
      Atomics.store(HEAP32, HEAD, head + 1);
    };
    const WM = { KEYDOWN: 0x100, KEYUP: 0x101, CHAR: 0x102, SYSKEYDOWN: 0x104, SYSKEYUP: 0x105,
      LBUTTONDOWN: 0x201, LBUTTONUP: 0x202, RBUTTONDOWN: 0x204, RBUTTONUP: 0x205, MBUTTONDOWN: 0x207,
      MBUTTONUP: 0x208, MOUSEWHEEL: 0x20A, XBUTTONDOWN: 0x20B, XBUTTONUP: 0x20C };
    const MSG = 1, FOCUSED = 2;

    const locked = () => document.pointerLockElement === canvas;
    const gameHasInput = () => locked() || document.activeElement === canvas;
    // the engine turns the view with the mouse (in a menu, its cursor follows the mouse instead)
    const mouseLook = () => Atomics.load(HEAP32, MOUSE_LOOK) !== 0;
    // the mouse is no one's, turning nothing with no cursor drawn for it: over the loading movie as the game loads
    // (src/client/cl_cin.cpp: CL_LoadingMovieHidesCursor), where the browser's arrow shows until a click captures it
    const mouseIdle = () => Atomics.load(HEAP32, MOUSE_IDLE) !== 0;
    // a click or a key captures the mouse
    const wantsMouse = () => mouseLook() || mouseIdle();

    // The mouse is captured (pointer lock) while the engine turns the view with it, as the Windows client's window
    // captures it, and is the player's own in a menu, which draws its cursor where the mouse is. In full screen the
    // page keeps it in a menu too, whose cursor moves as the mouse does (mousemove, below), as the Windows client's
    // full screen keeps it in the window: Esc opens the menu and closes it, the mouse the game's all the while. A click
    // captures it, and the game doesn't see that click: it would fire. A key does too (but Esc), which the game does
    // see. Chrome lets a page capture the mouse only just after one of those (within 5 s), in full screen, or when the
    // page let it go itself last. As a menu closes at a click (Resume, or the one that skips the loading movie), the
    // page captures the mouse again at once. In a window, one that closes at Esc, or a return to the window, leaves the
    // mouse free, the arrow over the game, until a click (which fires nothing) or a key: a player who pressed Esc
    // expects to click to come back in, as browsers have them do, and a capture they didn't see would have that click
    // fire (clicked, below).
    //
    // The click that captures it puts the page in full screen too: the page's own (Fullscreen API), where Keyboard Lock
    // (below) gives the page the game's keys and Esc, which Chrome's own full screen leaves with Chrome. F11 leaves it
    // and goes back (on a Mac, Control+Command+F too: fullScreenKey), and so does holding Esc. The click does so while
    // the player's Full Screen setting is on (fullScreenOn: bo1_web_fullscreen, web/src/web_main.cpp), which is one
    // setting however it is set: a player who leaves full screen in a game turns it off, and one who goes back turns it
    // on; the lobby's options menu sets it for the next game (zombies-hub's web/src/settings.ts); and the game's own
    // menu sets it in a game, its Graphics page's Full Screen, which the page follows into full screen or out of it
    // (followEngine). A gamepad's press goes full screen as the click does (padPressed, below).
    //
    // The player's first click on the game takes them into it, so: where a menu has the mouse then (the loading screen,
    // the loading movie's pregame menu), it only puts the page in full screen, and the game doesn't see it either. The
    // next click is the menu's ("LEFT MOUSE skip"). Over the loading movie as the game loads, where the mouse is no
    // one's, a click captures it, which hides the arrow, and the page lets it go as the pregame menu's cursor shows, at
    // the arrow's place: the cursor stays where the click was meanwhile.
    // the page has the player's attention: its window, its tab, and the game (the overlay down)
    const engaged = () => document.hasFocus() && !document.hidden && document.activeElement === canvas;
    // The player's Full Screen setting: as the page last set it, until the engine has taken that (it may be loading),
    // else as the engine has it.
    const fullScreenOn = () => {
      const set = Atomics.load(HEAP32, FULL_SCREEN_SET);
      return (set >= 0 ? set : Atomics.load(HEAP32, FULL_SCREEN)) !== 0;
    };
    // the player entered full screen, or left it, themselves: the setting is that from now on, which the engine keeps
    // in their config
    const setFullScreenOn = (on) => {
      if (fullScreenOn() !== on) Atomics.store(HEAP32, FULL_SCREEN_SET, on ? 1 : 0);
    };
    // The page's own change, as it follows the setting the engine changed (followEngine): full screen (true) or a window
    // (false), until it comes (fullscreenchange), else null. That change is no player's: the setting may have changed
    // again before it came (the row turned off and on again, Default System Settings), which it would undo.
    let following = null;
    const fullScreen = (on) => {
      (on ? document.documentElement.requestFullscreen() : document.exitFullscreen())
        .catch((e) => {
          following = null; // no change comes
          console.log('input: full screen failed: ' + e.message);
        });
    };
    // the click, key or gamepad's press that takes the player in goes full screen, while the setting is on
    const clickGoesFullScreen = () => !document.fullscreenElement && document.fullscreenEnabled && fullScreenOn();
    // byInput: on a click or key, which has the player's attention, and goes full screen. Not by itself (a menu closes
    // at a click) until the player is in the game (entered): Chrome would refuse it in a page just loaded, and in a page
    // whose last game let the mouse go it would let the page have the mouse with no click, which then went full screen
    // in none of the game
    const capture = (byInput) => {
      if (locked() || (!byInput && (!engaged() || !entered))) return;
      const goFullScreen = byInput && clickGoesFullScreen();
      // Chrome asks the OS for the mouse's own movement (no acceleration) where it can
      canvas.requestPointerLock({ unadjustedMovement: true })
        .catch((e) => (e && e.name === 'NotSupportedError' ? canvas.requestPointerLock() : Promise.reject(e)))
        .catch((e) => console.log('input: the mouse was not captured: ' + e.message))
        // full screen only once the mouse is asked for (twice where the platform has no raw movement): it uses up the
        // input's user activation, which asking for the mouse needs but leaves
        .then(() => goFullScreen && fullScreen(true));
    };
    let releasing = false; // the page let the mouse go (a menu opened), rather than Chrome
    let entered = false; // the player is in the game: a click took them in, or the mouse was captured, or full screen began
    const release = () => {
      releasing = true;
      document.exitPointerLock();
    };

    // In full screen, the game's keys are the page's, with Ctrl too (Ctrl+W closes the tab otherwise), and Esc, which
    // then doesn't let the mouse go: Chrome leaves full screen as it is held. Not the keys the system has shortcuts with
    // (Alt+Tab, Alt+F4, the Windows key, Print Screen), nor the locks, nor F11 and F12 (full screen, DevTools).
    const ownKeys = ['Tab', 'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight', 'ContextMenu', 'CapsLock', 'NumLock',
      'ScrollLock', 'PrintScreen', 'F4', 'F11', 'F12'];
    const lockKeys = () => {
      if (navigator.keyboard && navigator.keyboard.lock)
        navigator.keyboard.lock(Object.keys(bo1KeyTable).filter((code) => !ownKeys.includes(code))).catch(() => {});
    };
    lockKeys();
    let viewChanged = -Infinity; // the window changed size, or full screen began or ended, then (escaped, below)
    on(window, 'resize', () => {
      viewChanged = performance.now();
    });
    on(document, 'fullscreenchange', () => {
      viewChanged = performance.now();
      // the page's own change, as it followed the setting, sets nothing; where the setting changed again before the
      // change came, the page follows it again at its next frame (followEngine), as though the engine had what the
      // page now has
      const followed = following === !!document.fullscreenElement;
      following = null;
      if (followed && fullScreenOn() !== !!document.fullscreenElement) fullScreenWas = !!document.fullscreenElement;
      if (document.fullscreenElement) {
        lockKeys(); // again, in case Chrome forgot it as it left full screen
        // A mobile device shows the game on its side however it is held: the picture is wide (zombies-hub's
        // web/src/settings.ts: screenSize). Chrome on Android turns the page so in full screen alone, and back as the
        // page leaves it; a PC's refuses, having nothing to turn.
        if (window.screen.orientation && window.screen.orientation.lock)
          window.screen.orientation.lock('landscape').catch(() => {});
        if (!followed) setFullScreenOn(true);
        entered = true;
      } else {
        if (!followed && document.hasFocus() && document.activeElement === canvas) {
          // the player left it in the game (not the page as the game ended or as the setting went off, nor Chrome as
          // they switched away)
          setFullScreenOn(false);
        }
        // in a window, a menu's cursor follows the mouse, which is the player's (followEngine)
        if (locked() && !mouseLook() && !mouseIdle()) release();
      }
    });

    // a key to the engine, as Windows sends it: WM_KEYDOWN or WM_KEYUP (WM_SYSKEY* with Alt held), and for a key down,
    // the character it types (ch, 0 for none) in a WM_CHAR, as TranslateMessage would send it
    const sendKey = (code, down, repeat, altHeld, ch) => {
      const [vk, scan, ext] = bo1KeyTable[code];
      // lParam: repeat count 1, scan code, extended, previous state, transition
      let lParam = 1 | (scan << 16) | (ext << 24);
      if (!down) lParam |= (1 << 30) | (1 << 31);
      else if (repeat) lParam |= 1 << 30;
      Atomics.store(HEAP32, KEYS + vk, down ? 1 : 0);
      const alt = altHeld && vk !== 0x12;
      push(MSG, alt ? (down ? WM.SYSKEYDOWN : WM.SYSKEYUP) : (down ? WM.KEYDOWN : WM.KEYUP), vk, lParam | 0);
      if (down && !alt && ch) push(MSG, WM.CHAR, ch, lParam | 0);
    };
    // keys that let a page capture the mouse no more than Esc does
    const modifiers = ['Shift', 'Control', 'Alt', 'AltGraph', 'Meta', 'CapsLock', 'NumLock', 'ScrollLock'];
    // the player's last input was a click on the game, not a key: a menu that closes then has the mouse captured again
    // (followEngine, below)
    let clicked = false;
    // the keys that switch the page between full screen and a window: F11, and on a Mac, its own Control+Command+F
    const fullScreenKey = (e) => e.code === 'F11' || (e.code === 'KeyF' && e.ctrlKey && e.metaKey);
    const keyEvent = (e, down) => {
      const k = bo1KeyTable[e.code];
      if (!k || !gameHasInput()) return;
      // F12 (DevTools) is the browser's, and so is what goes down with Command held: a Mac's shortcuts, as the system
      // has the Windows key's. The game still sees a key's up, which went down before Command did.
      if (e.code === 'F12' || (down && e.metaKey && !fullScreenKey(e))) return;
      e.preventDefault();
      if (fullScreenKey(e)) {
        // the page's full screen, as the click's (above)
        if (down && !e.repeat) fullScreen(!document.fullscreenElement);
        return;
      }
      if (down && !e.repeat) clicked = false; // a key held as the player clicks is no newer than the click
      const activates = down && !e.repeat && e.code !== 'Escape' && !modifiers.includes(e.key);
      // a key takes the player in as a click does (the canvas' mousedown, below), the game seeing the key all the same.
      // Over a menu it captures the mouse too where it goes full screen, which keeps the mouse in a menu
      if (activates && !locked() && (wantsMouse() || !entered)) {
        entered = true;
        if (wantsMouse() || clickGoesFullScreen()) capture(true);
      }
      const vk = k[0];
      let ch = 0;
      if (e.key.length === 1) ch = e.key.charCodeAt(0);
      else if (vk === 0x08 || vk === 0x09 || vk === 0x0D || vk === 0x1B) ch = vk;
      sendKey(e.code, down, down && e.repeat, e.altKey, ch < 0x100 && !e.ctrlKey ? ch : 0);
    };
    on(window, 'keydown', (e) => keyEvent(e, true));
    on(window, 'keyup', (e) => keyEvent(e, false));

    let buttons = 0;
    let capturing = 0; // the buttons held since they captured the mouse: the game sees neither their down nor their up
    const MK = [0x0001, 0x0010, 0x0002, 0x0020, 0x0040]; // left, middle, right, x1, x2 (MouseEvent.button order)
    const mouseButton = (e, down) => {
      if (!gameHasInput()) return;
      e.preventDefault();
      const bit = MK[e.button];
      if (!bit) return;
      buttons = down ? (buttons | bit) : (buttons & ~bit);
      const msgs = [[WM.LBUTTONDOWN, WM.LBUTTONUP], [WM.MBUTTONDOWN, WM.MBUTTONUP], [WM.RBUTTONDOWN, WM.RBUTTONUP],
        [WM.XBUTTONDOWN, WM.XBUTTONUP], [WM.XBUTTONDOWN, WM.XBUTTONUP]][e.button];
      push(MSG, down ? msgs[0] : msgs[1], buttons, 0);
    };
    on(canvas, 'mousedown', (e) => {
      canvas.focus();
      clicked = true;
      if (!locked() && (wantsMouse() || !entered)) {
        e.preventDefault();
        capturing |= 1 << e.button;
        entered = true;
        if (wantsMouse()) capture(true);
        else if (clickGoesFullScreen()) fullScreen(true);
        return;
      }
      mouseButton(e, true);
    });
    on(window, 'mouseup', (e) => {
      if (capturing & (1 << e.button)) {
        capturing &= ~(1 << e.button);
        e.preventDefault();
        return;
      }
      mouseButton(e, false);
    });
    on(canvas, 'contextmenu', (e) => e.preventDefault());
    on(canvas, 'wheel', (e) => {
      if (!gameHasInput() || (!locked() && mouseLook())) return;
      e.preventDefault();
      push(MSG, WM.MOUSEWHEEL, ((e.deltaY < 0 ? 120 : -120) << 16) | buttons, 0);
    }, { passive: false });
    // the picture's scale in the canvas' box, which it keeps its shape in (zombies-hub's web/src/page.css: object-fit),
    // with bars at its sides or above and below: CSS pixels to one of the back buffer's
    const pictureScale = (r) => Math.min(r.width / canvas.width, r.height / canvas.height);
    // what a menu's cursor moved, the mouse captured, of a pixel more than it shows (mousemove)
    let menuRest = [0, 0];
    on(window, 'mousemove', (e) => {
      if (locked()) {
        // a mouse that is no one's moves no cursor (above)
        if ((!e.movementX && !e.movementY) || mouseIdle()) return;
        if (mouseLook()) {
          Atomics.add(HEAP32, CURSOR_X, e.movementX | 0);
          Atomics.add(HEAP32, CURSOR_Y, e.movementY | 0);
        } else {
          // a menu's cursor, the mouse kept captured in full screen (followEngine, below): it moves as the mouse
          // would move it over the picture, not captured (below), and stays on the picture
          const s = pictureScale(canvas.getBoundingClientRect());
          const axes = [[CURSOR_X, e.movementX, canvas.width], [CURSOR_Y, e.movementY, canvas.height]];
          axes.forEach(([at, d, n], i) => {
            const v = Atomics.load(HEAP32, at) + d / s + menuRest[i];
            const shown = Math.max(0, Math.min(n - 1, Math.round(v)));
            menuRest[i] = shown === Math.round(v) ? v - shown : 0;
            Atomics.store(HEAP32, at, shown);
          });
        }
      } else {
        // the mouse not captured turns no view: it is the player's, on its way to the click that captures it
        if (mouseLook()) return;
        // a menu's cursor: where the mouse is on the picture
        const r = canvas.getBoundingClientRect();
        const s = pictureScale(r);
        const x = Math.round((e.clientX - r.left - (r.width - canvas.width * s) / 2) / s);
        const y = Math.round((e.clientY - r.top - (r.height - canvas.height * s) / 2) / s);
        const wasX = Atomics.exchange(HEAP32, CURSOR_X, x), wasY = Atomics.exchange(HEAP32, CURSOR_Y, y);
        if (wasX === x && wasY === y) return;
      }
      // when it moved, on emscripten_get_now's clock, after the movement itself: the frame rate overlay's mouse lag
      // (web_platform.h: inputTime)
      HEAPF64[INPUT_TIME] = performance.timeOrigin + e.timeStamp;
    });
    const focusChanged = () => {
      const has = document.hasFocus() || locked();
      if (Atomics.load(HEAP32, FOCUS) === (has ? 1 : 0)) return;
      Atomics.store(HEAP32, FOCUS, has ? 1 : 0);
      if (!has) {
        // release everything held, as Windows does on deactivation
        for (let vk = 0; vk < 256; vk++) Atomics.store(HEAP32, KEYS + vk, 0);
        buttons = 0;
      }
      push(FOCUSED, has ? 1 : 0, 0, 0);
    };
    // back from another window or tab, the mouse is the player's until they click (above)
    on(window, 'focus', focusChanged);
    on(window, 'blur', focusChanged);
    // Chrome let the mouse go on Esc, which it keeps from the page where the keyboard isn't the page's (not in full
    // screen): the engine has the Esc, for its menu, as the Windows client would. Not when it let the mouse go as the
    // window or the tab lost the player (they switched to another), or as full screen began or ended.
    const escaped = () => {
      if (locked() || !mouseLook() || !engaged() || performance.now() - viewChanged < 1000) return;
      console.log('input: Chrome let the mouse go on Esc: the game has it');
      clicked = false; // a key the page didn't see, the player's last input all the same
      sendKey('Escape', true, false, false, 0x1B);
      sendKey('Escape', false, false, false, 0);
    };
    on(document, 'pointerlockchange', () => {
      Atomics.store(HEAP32, LOCKED, locked() ? 1 : 0);
      if (locked()) entered = true;
      focusChanged();
      console.log('input: ' + (locked() ? 'the mouse is captured' : releasing ? 'the page let the mouse go'
        : 'Chrome let the mouse go'));
      if (locked()) return;
      // the window loses focus, or full screen ends, just after the mouse goes: a moment for them to
      if (!releasing) setTimeout(escaped, 250);
      releasing = false;
    });

    // each animation frame, what the engine does with the mouse: as it starts to turn the view, the page captures the
    // mouse (above), and as a menu opens, lets it go, as it does one that was no one's as a menu's cursor shows; the
    // system cursor hides over the canvas while the engine draws its own (a menu's)
    let looked = false;
    let idled = false;
    let cursor = '';
    let fullScreenWas = Atomics.load(HEAP32, FULL_SCREEN) !== 0; // the Full Screen setting as the engine last had it
    // the gamepad plays (pollPads, below): the arrow, there for the click that captures the mouse as the view turns,
    // hides over the canvas, from a press or a push of the gamepad until the mouse moves
    let padPlays = false;
    on(canvas, 'mousemove', () => { padPlays = false; });
    const followEngine = () => {
      if (signal.aborted) return;
      const look = mouseLook();
      const idle = mouseIdle();
      if (look !== looked) {
        looked = look;
        console.log(look ? 'input: the mouse turns the view' : 'input: the mouse is a menu\'s');
        menuRest = [0, 0];
        if (look && clicked) capture(false);
        else if (!look && !idle && locked() && !document.fullscreenElement) release();
      }
      if (idle !== idled) {
        idled = idle;
        console.log(idle ? 'input: the mouse is no one\'s' : 'input: the mouse is no one\'s no more');
        if (!idle && !look && locked() && !document.fullscreenElement) release();
      }
      const c = (look && !padPlays) || Atomics.load(HEAP32, CURSOR_SHOWN) ? '' : 'none';
      if (c !== cursor) canvas.style.cursor = cursor = c;
      // The Full Screen setting changed in the engine: the game's menu set it (its Graphics page's row), and the page
      // goes full screen, as a click on the game does (with the mouse, captured first, which full screen keeps in a
      // menu), or leaves it. Chrome lets it in within 5 s of the click, key or press that set the row; later, the next
      // one that takes the player in goes full screen (clickGoesFullScreen). What the player set by entering or
      // leaving full screen themselves comes back here too, to a page already as it says.
      const on = Atomics.load(HEAP32, FULL_SCREEN) !== 0;
      if (on !== fullScreenWas) {
        fullScreenWas = on;
        if (!on && document.fullscreenElement) {
          following = false;
          fullScreen(false);
        } else if (on && entered && engaged() && clickGoesFullScreen() &&
          !(navigator.userActivation && !navigator.userActivation.isActive)) {
          following = true;
          if (!locked() && !padPlays) capture(true);
          else fullScreen(true);
        }
      }
      requestAnimationFrame(followEngine);
    };
    requestAnimationFrame(followEngine);

    // gamepads: copy the standard mapping into XINPUT_GAMEPAD every frame (web_input.cpp: Bo1GamepadShared). The
    // engine reads the first of them alone (src/win32/win_gamepad.cpp: one port), so the gamepads with the standard
    // mapping go in Chrome's order, the first first, whatever Chrome numbers them: another device (a wheel, a pad
    // Chrome can't map) may have the first number. Chrome shows a page a gamepad once one of its buttons is pressed
    // (the console says each, from the lobby on: zombies-hub's web/src/padnav.ts).
    const PAD_CONNECTED = (shared + gamepadOffset) >> 2;
    const PAD_PACKET = PAD_CONNECTED + 4;
    const PAD_BASE = shared + gamepadOffset + 32; // XINPUT_GAMEPAD[4], 12 bytes each
    // The motors' speeds the engine sets (web_input.cpp: XInputSetState), which the gamepad plays as Chrome's dual
    // rumble: the left motor the strong one, the right the weak one. Again as they change, and every 100 ms while they
    // run, each for 250 ms, so a page that stops polling (a hidden tab, the game gone) leaves them running a moment at
    // most; a reset as they stop.
    const PAD_MOTORS = (shared + gamepadOffset + 80) >> 1; // uint16_t[4][2]
    const rumbles = [null, null, null, null]; // what each plays: its strong and weak magnitudes, and since when
    let rumbled = false;
    const rumble = (p, i) => {
      const act = p.vibrationActuator;
      const strong = HEAPU16[PAD_MOTORS + i * 2] / 65535;
      const weak = HEAPU16[PAD_MOTORS + i * 2 + 1] / 65535;
      const last = rumbles[i];
      if (!strong && !weak) {
        if (last && act) act.reset().catch(() => {});
        rumbles[i] = null;
        return;
      }
      const now = performance.now();
      if (last && Math.abs(last[0] - strong) < 0.02 && Math.abs(last[1] - weak) < 0.02 && now - last[2] < 100) return;
      rumbles[i] = [strong, weak, now];
      if (!rumbled) {
        rumbled = true;
        console.log(act ? 'input: the gamepad rumbles' : 'input: the gamepad would rumble, but Chrome has no way to make it');
      }
      if (act) act.playEffect('dual-rumble', { duration: 250, strongMagnitude: strong, weakMagnitude: weak }).catch(() => {});
    };
    // A gamepad's press is a gesture, as a click or a key is: Chrome counts a press that getGamepads() sees as user
    // activation. The page hears of it (bo1-padpress, zombies-hub's web/src/gestures.ts: GESTURES), which starts the
    // game's sound and the loading movie's (above), and it takes the player into the game as a click does (the canvas'
    // mousedown, above):
    // it goes full screen, capturing the mouse first where the view turns with it, while the Full Screen setting is
    // on. Where Chrome doesn't count it (a trigger pressed only part of the way), the press goes on to the game
    // alone. The game sees the press either way, as it sees a key that captures the mouse.
    const padPressed = () => {
      document.dispatchEvent(new Event('bo1-padpress'));
      if (!engaged() || !clickGoesFullScreen()) return;
      if (navigator.userActivation && !navigator.userActivation.isActive) return;
      entered = true;
      if (wantsMouse() && !locked()) capture(true);
      else fullScreen(true);
    };
    let padsDown = null; // each gamepad's buttons held, a bit each, as last read: what is held as the game starts (the
    // A that said Ready) is no press
    const XB = [0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0020, 0x0010, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008];
    let packet = 0;
    const pollPads = () => {
      if (signal.aborted) return;
      const pads = [...(navigator.getGamepads ? navigator.getGamepads() : [])]
        .filter((p) => p && p.connected && p.mapping === 'standard');
      const down = pads.map((p) => p.buttons.reduce((bits, b, i) => (b.pressed ? bits | (1 << i) : bits), 0));
      if (padsDown && down.some((bits, i) => bits & ~(padsDown[i] || 0))) padPressed();
      padsDown = down;
      for (let i = 0; i < 4; i++) {
        const p = pads[i];
        if (!p) {
          Atomics.store(HEAP32, PAD_CONNECTED + i, 0);
          rumbles[i] = null;
          continue;
        }
        let wButtons = 0;
        for (let b = 0; b < XB.length && b < p.buttons.length; b++) if (p.buttons[b].pressed) wButtons |= XB[b];
        if (i === 0 && (p.buttons.some((b) => b.pressed) || p.axes.some((v) => Math.abs(v) > 0.5))) padPlays = true;
        const base = PAD_BASE + i * 12;
        const stick = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
        HEAPU16[base >> 1] = wButtons;
        HEAPU8[base + 2] = Math.round((p.buttons[6] ? p.buttons[6].value : 0) * 255);
        HEAPU8[base + 3] = Math.round((p.buttons[7] ? p.buttons[7].value : 0) * 255);
        HEAP16[(base + 4) >> 1] = stick(p.axes[0] || 0);
        HEAP16[(base + 6) >> 1] = stick(-(p.axes[1] || 0));
        HEAP16[(base + 8) >> 1] = stick(p.axes[2] || 0);
        HEAP16[(base + 10) >> 1] = stick(-(p.axes[3] || 0));
        Atomics.store(HEAP32, PAD_PACKET + i, ++packet);
        Atomics.store(HEAP32, PAD_CONNECTED + i, 1);
        rumble(p, i);
      }
      requestAnimationFrame(pollPads);
    };
    requestAnimationFrame(pollPads);
    canvas.tabIndex = 0;
    canvas.focus();
  },

  // ---- the player's folder's file system backend ----
  // Emscripten's OPFS backend (libwasmfs_opfs.js) keeps the engine's /home/players (web/src/web_main.cpp:
  // Bo1_MountFileSystem), the player's config, in the browser's store (zombies-hub's web/public/store.js): its root is
  // the store's players/ directory, in place of the store itself, which Emscripten's own would make the root. On the
  // backend's own thread.
  // A store the browser denies leaves the root unset: each open of the folder fails then, and the engine says it
  // couldn't write its config.
  _wasmfs_opfs_init_root_directory__deps: ['$wasmfsOPFSDirectoryHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_init_root_directory: async (ctx) => {
    // allocated.length starts off as 1 since 0 is a reserved handle (libwasmfs_opfs.js)
    if (wasmfsOPFSDirectoryHandles.allocated.length == 1) {
      let players = null;
      try {
        players = await (await navigator.storage.getDirectory()).getDirectoryHandle('players', { create: true });
      } catch (e) {
        console.error('store: the player\'s folder could not be opened: ' + e.message);
      }
      wasmfsOPFSDirectoryHandles.allocated.push(players);
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  // ---- the install's file system backend ----
  // The engine reads the install (web/src/web_main.cpp: Bo1_MountFileSystem) through Emscripten's fetch backend's C++
  // (wasmfs_create_fetch_backend) with this in place of its JS (libwasmfs_fetch.js), on the backend's own thread: from
  // the browser's store alone (zombies-hub's web/public/store.js), which a map's download filled before its game
  // (zombies-hub's web/public/player.js: store), never from the server. A file is the manifest's
  // (path<tab>size<tab>hash, zombies-hub's server/web.go), read through the store, where a game offline finds it; the
  // store keeps it in pieces by its hash, those the map's list names (zombies-hub's server/files.go), in its packs,
  // whose indexes it reads as the backend starts. A read gets the bytes the store has from where it starts, up to its
  // length, each piece's from its pack into the engine's memory: fewer where the store has no more of them just then
  // (the engine's archive cache, web/src/crt_compat.cpp, asks for more than it needs, past the entry it wants). A read
  // the store has nothing at fails, and the page ends the game, saying what the store lacks (bo1_store_missing): the
  // map's list missed it, or the browser dropped part of the store. A read fails too where the store doesn't answer it
  // in time (answered), or throws: Emscripten's glue (libwasmfs_jsimpl.js) answers the engine only once a read returns,
  // so a read that never does, or throws, would leave the engine waiting for good.
  _wasmfs_create_fetch_backend_js__deps: ['$wasmFS$backends', '_wasmfs_fetch_get_file_url', 'bo1_store_missing',
    'bo1_store_hung', '$stringToNewUTF8', 'free'],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    if (typeof bo1Store === 'undefined') importScripts('/store.js');
    bo1Store.index().catch(() => {}); // the packs' indexes, read as the engine starts (failing, its first read again)
    const files = new Map(); // the file's pointer -> { path, size, hash, pieces }
    const errno = (e) => (e && e.status === 404 ? -44 /* ENOENT */ : -29 /* EIO */);
    async function info(file) {
      let f = files.get(file);
      if (f) return f;
      // 'game/<version>/<path>': the path has the backend's directory (main/) and the file's
      const url = UTF8ToString(__wasmfs_fetch_get_file_url(file)).replace(/\/+/g, '/');
      const m = /^game\/(v[0-9a-f]{16})\/(.+)$/.exec(url);
      if (!m) throw { status: 404 };
      const e = (await answered(bo1Store.manifest(m[1]), m[2])).get(m[2].toLowerCase());
      if (!e) throw { status: 404 };
      f = { path: m[2], size: e.size, hash: e.hash, pieces: null };
      files.set(file, f);
      return f;
    }
    // the piece of the file's that has the byte at, or none; pieces are by start, and may overlap a little (an
    // archive's last entry and its end, which the engine reads for its directory)
    function pieceAt(f, at) {
      let lo = 0, hi = f.pieces.length - 1, found = -1;
      while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (f.pieces[mid].start <= at) {
          found = mid;
          lo = mid + 1;
        } else {
          hi = mid - 1;
        }
      }
      for (let i = found; i >= 0 && i > found - 3; i--) if (f.pieces[i].end > at) return f.pieces[i];
      return null;
    }
    // the page hears once why the game ends: the store lacks what a read wanted (missing), or hung (answered)
    let told = false;
    function missing(f, at) {
      console.error('store: no piece of ' + f.path + ' (' + f.hash + ') at byte ' + at);
      if (told) return;
      told = true;
      const path = stringToNewUTF8(f.path);
      _bo1_store_missing(path, at);
      _free(path);
    }
    // p, unless the store hasn't answered it in 30 s: a step of the browser's file system hung, as a download's can
    // (zombies-hub's web/public/store.js: watchdog), and the engine would wait on it for good. It fails then
    // ({ hung: true }), and the page ends the game, saying the browser stopped answering (bo1_store_hung); the game
    // started again has a new page's workers.
    function answered(p, path) {
      let timer;
      const hung = new Promise((resolve, reject) => {
        timer = setTimeout(() => {
          console.error('store: no answer in 30 s, reading ' + path);
          if (!told) {
            told = true;
            const text = stringToNewUTF8(path);
            _bo1_store_hung(text);
            _free(text);
          }
          reject({ hung: true });
        }, 30000);
      });
      return Promise.race([p, hung]).finally(() => clearTimeout(timer));
    }
    wasmFS$backends[backend] = {
      allocFile: async (file) => {},
      freeFile: async (file) => {
        files.delete(file);
      },
      write: async () => -29,
      setSize: async () => -29,
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        let f;
        try {
          f = await info(file);
          if (!f.pieces) f.pieces = await answered(bo1Store.pieces(f.hash), f.path);
        } catch (e) {
          return errno(e);
        }
        length = Math.min(length, f.size - offset);
        if (length <= 0) return 0;
        let done = 0;
        while (done < length) {
          const at = offset + done;
          const p = pieceAt(f, at);
          const n = p ? Math.min(p.end - at, length - done) : 0;
          let ok = false;
          try {
            const into = HEAPU8.subarray(buffer + done, buffer + done + n);
            ok = !!p && await answered(bo1Store.readPiece(p, at - p.start, into), f.path);
          } catch (e) {
            if (e && e.hung) return -29; // EIO: the page heard (answered)
            // else as when the store has the piece no more
          }
          if (!ok) break;
          done += n;
        }
        if (!done) {
          missing(f, offset);
          return -29;
        }
        return done;
      },
      getSize: async (file) => {
        try {
          return (await info(file)).size;
        } catch (e) {
          return 0;
        }
      },
    };
  },
});
