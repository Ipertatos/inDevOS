import * as os from 'qjs:os';
const parent = os.Worker.parent;
parent.onmessage = (e) => { const ia = new Int32Array(e.data.sab); let s = 0; for (let i = 0; i < 100000; i++) { Atomics.add(ia, 0, 1); s += i; } Atomics.notify(ia, 0); const wa = new Int32Array(new SharedArrayBuffer(4)); const a = Date.now(); const r = Atomics.wait(wa, 0, 0, 100); parent.postMessage({ type: 'done', sum: s, w: r, wd: Date.now() - a }); parent.onmessage = null; };
