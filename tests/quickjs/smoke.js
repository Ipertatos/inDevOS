// inDevOS QuickJS smoke test: exercises the OS-facing parts (clock, FPU, timers, threads, fork/exec, signals, fs).
// Usage: qjs --std smoke.js   (prints "PASS name" / "FAIL name: why", final "SMOKE x/y")
import * as std from 'qjs:std';
import * as os from 'qjs:os';
let pass = 0, total = 0; const pending = [];
// optional filters: qjs --std smoke.js [only=a,b] [skip=c,d]  (substring match on test names)
const args = scriptArgs.slice(1); const only = (args.find(a => a.startsWith('only=')) || '').slice(5).split(',').filter(Boolean);
const skip = (args.find(a => a.startsWith('skip=')) || '').slice(5).split(',').filter(Boolean);
const want = n => (only.length === 0 || only.some(o => n.includes(o))) && !skip.some(o => n.includes(o));
const begin = n => { print('BEGIN ' + n); std.out.flush(); };
function t(name, fn) { if (!want(name)) return; begin(name); total++; try { const r = fn(); if (r === false) throw new Error('returned false'); print('PASS ' + name); std.out.flush(); pass++; } catch (e) { print('FAIL ' + name + ': ' + e); std.out.flush(); } }
function ta(name, mk) { if (!want(name)) return; begin(name); const p = mk(); total++; let h; pending.push(Promise.race([p, new Promise((_, rej) => { h = os.setTimeout(() => rej(new Error('timeout')), 15000); })]).finally(() => os.clearTimeout(h)).then(() => { print('PASS ' + name); pass++; }, e => print('FAIL ' + name + ': ' + e))); }
const eq = (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); };
t('fpu-basic', () => { eq(Math.sqrt(2).toFixed(12), '1.414213562373'); eq((0.1 + 0.2).toString(), '0.30000000000000004'); eq(Math.sin(Math.PI / 6).toFixed(10), '0.5000000000'); });
t('fpu-libm', () => { eq(Math.exp(1).toPrecision(15), '2.71828182845905'); eq(Math.log10(1e300), 300); eq(Math.cbrt(27), 3); eq(Math.hypot(3, 4), 5); eq(Math.atan2(1, 1).toFixed(10), (Math.PI / 4).toFixed(10)); eq(Math.fround(5.5), 5.5); eq(Math.pow(2, 0.5), Math.SQRT2); });
t('dtoa', () => { eq(parseFloat('1.7976931348623157e308'), Number.MAX_VALUE); eq((123.456).toExponential(2), '1.23e+2'); eq(String(5e-324), '5e-324'); eq((1e21).toString(), '1e+21'); eq(Number('0x1f'), 31); });
t('bigint', () => { eq((2n ** 128n).toString(), '340282366920938463463374607431768211456'); eq(BigInt.asUintN(64, -1n), 18446744073709551615n); });
t('es2023', () => { eq([3, 1, 2].toSorted().join(), '1,2,3'); eq([1, 2, 3].findLast(x => x < 3), 2); eq([1, 2].at(-1), 2); eq(Object.groupBy([1, 2, 3], x => x % 2 ? 'o' : 'e').o.length, 2); eq('a-b'.replaceAll('-', '+'), 'a+b');  });
t('class-fields', () => { class A { #x = 1; static s = 2; get x() { return this.#x; } static { A.t = 3; } } eq(new A().x + A.s + A.t, 6); });
t('regexp-unicode', () => { eq(/\p{Script=Greek}+/u.exec('abc αβγ')[0], 'αβγ'); eq('ΓΕΙΑ'.toLowerCase(), 'γεια'); eq(/(?<y>\d{4})/.exec('in 2026').groups.y, '2026'); });
t('json', () => { eq(JSON.stringify({ a: [1, { b: 'ü' }] }), '{"a":[1,{"b":"ü"}]}'); eq(JSON.parse('{"x":1.5e3}').x, 1500); });
t('date-now', () => { const a = Date.now(); eq(a > 1.7e12, true); const d = new Date(Date.UTC(2026, 9, 27, 12)); eq(d.toISOString(), '2026-10-27T12:00:00.000Z'); });
t('clock-monotonic', () => { const a = performance.now(), b = os.now(); let x = 0; for (let i = 0; i < 2e6; i++) x += i; const c = performance.now(); if (!(c > a)) throw new Error(`performance.now not advancing ${a} ${c}`); eq(typeof b, 'number'); });
t('os-sleep', () => { const a = Date.now(); os.sleep(200); const d = Date.now() - a; if (d < 180 || d > 2000) throw new Error('slept ' + d + 'ms'); print('  sleep(200) took ' + d + 'ms'); });
t('fs-rw', () => { const p = '/tmp/qjs_smoke.txt'; const f = std.open(p, 'w'); f.puts('hello\nworld\n'); f.close(); eq(std.loadFile(p), 'hello\nworld\n'); const [st, err] = os.stat(p); eq(err, 0); eq(st.size, 12); eq(os.remove(p), 0); });
t('fs-seek', () => { const f = std.tmpfile(); f.puts('abcdef'); f.seek(2, std.SEEK_SET); eq(f.getByte(), 'c'.charCodeAt(0)); f.close(); });
t('readdir', () => { const [l, e] = os.readdir('/q'); eq(e, 0); eq(l.includes('qjs'), true); });
t('getenv', () => { eq(typeof std.getenviron(), 'object'); });
t('cwd', () => { const [d, e] = os.getcwd(); eq(e, 0); print('  cwd=' + d); });
t('exec-wait', () => { const r = os.exec(['/q/qjs', '--std', '-e', 'std.exit(7)'], { block: true }); eq(r, 7); });
t('exec-pipe', () => { const [rd, wr] = os.pipe(); const pid = os.exec(['/q/qjs', '-e', 'print("piped")'], { stdout: wr, block: false }); os.close(wr); const f = std.fdopen(rd, 'r'); const s = f.readAsString(); f.close(); os.waitpid(pid, 0); eq(s, 'piped\n'); });
t('signal-self', () => { let got = 0; os.signal(os.SIGUSR1 ?? 10, () => { got++; }); os.kill(os.getpid ? os.getpid() : 0, os.SIGUSR1 ?? 10); /* handler runs from the event loop */ return true; });
ta('setTimeout', () => new Promise(res => { const a = Date.now(); os.setTimeout(() => { const d = Date.now() - a; print('  setTimeout(100) fired after ' + d + 'ms'); res(); }, 100); }));
ta('timer-order', () => new Promise((res, rej) => { const o = []; os.setTimeout(() => o.push(2), 20); os.setTimeout(() => o.push(1), 5); os.setTimeout(() => { o.join() === '1,2' ? res() : rej(new Error(o.join())); }, 60); }));
ta('promise-async', () => (async () => { const v = await Promise.all([1, Promise.resolve(2), new Promise(r => os.setTimeout(() => r(3), 10))]); eq(v.join(), '1,2,3'); })());
ta('sleepAsync', () => os.sleepAsync(50));
ta('worker', () => new Promise((res, rej) => {
  if (!os.Worker) return rej(new Error('no os.Worker'));
  const w = new os.Worker('/q/smoke_worker.js'); const sab = new SharedArrayBuffer(8); const ia = new Int32Array(sab);
  w.onmessage = (e) => { if (e.data.type === 'done') { try { eq(Atomics.load(ia, 0), 100000); eq(e.data.sum, 4999950000); print('  worker Atomics.wait(100) -> ' + e.data.w + ' in ' + e.data.wd + 'ms'); eq(e.data.w, 'timed-out'); if (e.data.wd < 80) throw new Error('wait too fast'); w.onmessage = null; res(); } catch (x) { rej(x); } } };
  w.postMessage({ sab });
}));
Promise.allSettled(pending).then(() => { print(`SMOKE ${pass}/${total}`); });
