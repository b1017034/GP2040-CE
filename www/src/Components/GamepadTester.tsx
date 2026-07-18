// Live tester for the HML board, styled after analog-stick config tools like
// Firebird: two round "scope" fields (octagon gate, crosshair, radial ticks,
// fading trail, glowing dot, X/Y/magnitude/angle readout), a per-stick magnitude
// waveform (the Firebird snapback-style trace), trigger meters and a button row.
import { useEffect, useRef, useState } from 'react';

type RawData = {
	channels: number[];
	adc: number[];
	gpio?: number;
	valid: boolean;
};

type Opts = {
	chLX: number; chLY: number; chRX: number; chRY: number;
	chPsButton: number; psThreshold: number; psActiveHigh: boolean;
	l3Pin: number; r3Pin: number;
	chL3?: number; l3Threshold?: number; l3ActiveHigh?: boolean;
	chR3?: number; r3Threshold?: number; r3ActiveHigh?: boolean;
	leftStick: any; rightStick: any;
};

const PIN_BUTTONS: Record<number, string> = {
	23: 'up', 8: 'down', 16: 'left', 17: 'right',
	15: 'b1', 9: 'b2', 13: 'b3', 14: 'b4',
	22: 'l1', 21: 'r1', 18: 's1', 19: 's2', 12: 'tp',
};

const LT = { idle: 4060, pressed: 20 };
const RT = { idle: 2057, pressed: 20 };

const ACCENT = '#2fd6ff';
const ACCENT2 = '#7c5cff';
const ON = '#ffcf5c';
const INK = 'var(--bs-body-color, #c9d1d9)';
const PANEL = 'var(--bs-tertiary-bg, #12161c)';
const LINE = 'var(--bs-border-color, #313842)';

function pinDown(gpio: number | undefined, pin: number): boolean {
	if (gpio === undefined || pin < 0) return false;
	return ((gpio >>> pin) & 1) === 0;
}

function calAxis(raw: number, cal: { mn: number; ctr: number; mx: number }, invert: boolean): number {
	let v: number;
	if (cal.mx > cal.mn && cal.ctr > cal.mn && cal.mx > cal.ctr) {
		v = raw >= cal.ctr ? (raw - cal.ctr) / (cal.mx - cal.ctr) : -(cal.ctr - raw) / (cal.ctr - cal.mn);
	} else {
		v = (raw - 32768) / 32768;
	}
	return Math.max(-1, Math.min(1, invert ? -v : v));
}

function trigTravel(raw: number, c: { idle: number; pressed: number }): number {
	return Math.max(0, Math.min(1, (c.idle - raw) / (c.idle - c.pressed)));
}

// Mirror of the firmware stick pipeline (hml_analog.cpp processStick), minus
// the time-domain RC filter: radial deadzones -> sensitivity curve -> diagonal
// scaling. Lets the tester show the value the game actually receives.
const CURVE_PRESETS = [
	[[30, 30], [70, 70], [100, 100]],
	[[60, 30], [90, 80], [100, 100]],
	[[30, 60], [70, 90], [100, 100]],
	[[20, 80], [60, 95], [100, 100]],
	[[30, 25], [70, 75], [100, 100]],
];

export function applyCurve(mag: number, s: any): number {
	const preset = s.curvePreset ?? 0;
	let pts: number[][];
	if (preset >= 5) {
		let count = Math.min(s.curveX?.length ?? 0, 10);
		if (s.curvePointCount > 0 && s.curvePointCount < count) count = s.curvePointCount;
		if (!count) return mag;
		pts = Array.from({ length: count }, (_, i) => [(s.curveX[i] ?? 100) / 100, (s.curveY[i] ?? 100) / 100]);
	} else {
		pts = (CURVE_PRESETS[preset] ?? CURVE_PRESETS[0]).map(([px, py]) => [px / 100, py / 100]);
	}
	let px = 0, py = 0;
	for (const [nx, ny] of pts) {
		if (mag <= nx) {
			const denom = nx - px;
			return denom <= 0.0001 ? ny : py + (mag - px) * (ny - py) / denom;
		}
		px = nx; py = ny;
	}
	return py > 0 ? py : mag;
}

const pct = (v: number, d: number) => (v === undefined || v === null ? d : v) / 100;

function axisDeadzone(v: number, innerPct: number, outerPct: number): number {
	const inner = (innerPct ?? 0) / 100;
	let outer = (outerPct ?? 100) === 0 ? 1 : (outerPct ?? 100) / 100;
	if (outer <= inner + 0.001) outer = inner + 0.001;
	const av = Math.abs(v);
	if (av <= inner) return 0;
	const nv = Math.min(1, (av - inner) / (outer - inner));
	return v >= 0 ? nv : -nv;
}

function antiDeadzone(v: number, antiPct: number): number {
	if (!antiPct || Math.abs(v) < 0.0001) return v;
	const anti = antiPct / 100;
	const av = Math.min(1, anti + Math.abs(v) * (1 - anti));
	return v >= 0 ? av : -av;
}

// Mirror of firmware processStick (new FireBird pipeline), minus the
// time-domain RC/snapback filters and the resolution/delay output stage.
export function processStick(x: number, y: number, s: any): { x: number; y: number } {
	// per-direction input scale
	x *= x >= 0 ? pct(s.scaleRight, 100) : pct(s.scaleLeft, 100);
	y *= y >= 0 ? pct(s.scaleUp, 100) : pct(s.scaleDown, 100);
	// per-axis deadzones
	x = axisDeadzone(x, s.innerX ?? 0, s.outerX ?? 100);
	y = axisDeadzone(y, s.innerY ?? 0, s.outerY ?? 100);
	const mag = Math.hypot(x, y);
	if (mag < 0.0001) return { x: 0, y: 0 };
	let outMag = applyCurve(Math.min(mag, 1), s);
	let ca = x / mag, sa = y / mag;
	// angle correction (offset + snap); angle map omitted in the mirror
	const off = (s.angleOffset100 ?? 0) / 100, snap = s.angleSnap ?? 0;
	if (off !== 0 || snap !== 0) {
		let ang = Math.atan2(ca, sa) * 180 / Math.PI; // 0=up, cw
		if (ang < 0) ang += 360;
		ang += off;
		if (snap === 1) ang = Math.round(ang / 45) * 45;
		else if (snap === 2) ang = Math.round(ang / 90) * 90;
		const rad = ang * Math.PI / 180;
		ca = Math.sin(rad); sa = Math.cos(rad);
	}
	const dx = s.diagScaleX ?? 0, dy = s.diagScaleY ?? 0;
	if (dx > 0 || dy > 0) {
		const maxSquare = Math.min(1 / Math.max(Math.abs(ca), 1e-6), 1 / Math.max(Math.abs(sa), 1e-6));
		const ex = 1 + 0.4142 * dx / 100, ey = 1 + 0.4142 * dy / 100;
		const maxEllipse = 1 / Math.sqrt(Math.max((ca * ca) / (ex * ex) + (sa * sa) / (ey * ey), 1e-9));
		outMag *= Math.min(maxSquare, maxEllipse);
	}
	let ox = Math.max(-1, Math.min(1, ca * outMag));
	let oy = Math.max(-1, Math.min(1, sa * outMag));
	ox = antiDeadzone(ox, s.antiX ?? 0);
	oy = antiDeadzone(oy, s.antiY ?? 0);
	return { x: ox, y: oy };
}

const fmt = (v: number) => `${v >= 0 ? '+' : ''}${v.toFixed(2)}`;

// Round scope for one stick: smoothed dot + fading trail + magnitude waveform.
// `x`/`y` are in [-1,1] with +y = up (screen up).
function StickScope({
	label, x, y, pressed, stick,
}: { label: string; x: number; y: number; pressed: boolean; stick: any }) {
	const inner = stick.innerX ?? 0;
	const outer = 100 - (stick.outerX ?? 100);
	const diagX = stick.diagScaleX ?? 0;
	const diagY = stick.diagScaleY ?? 0;
	const stickRef = useRef(stick);
	stickRef.current = stick;
	const target = useRef({ x, y });
	target.current = { x, y };
	const [pos, setPos] = useState({ x, y });
	const trail = useRef<{ x: number; y: number }[]>([]);
	const wave = useRef<number[]>(new Array(120).fill(0));
	const [, force] = useState(0);

	useEffect(() => {
		let frame = 0;
		const tick = () => {
			setPos((p) => {
				const nx = p.x + (target.current.x - p.x) * 0.35;
				const ny = p.y + (target.current.y - p.y) * 0.35;
				// trail/waveform follow the corrected output, not the raw input
				const c = processStick(nx, ny, stickRef.current);
				const t = trail.current;
				const last = t[t.length - 1];
				if (!last || Math.hypot(last.x - c.x, last.y - c.y) > 0.015) {
					t.push(c);
					if (t.length > 44) t.shift();
				}
				const w = wave.current;
				w.push(Math.min(1, Math.hypot(c.x, c.y)));
				if (w.length > 120) w.shift();
				return { x: nx, y: ny };
			});
			force((n) => (n + 1) & 0xffff);
			frame = requestAnimationFrame(tick);
		};
		frame = requestAnimationFrame(tick);
		return () => cancelAnimationFrame(frame);
	}, []);

	const S = 200, C = S / 2, R = 88, dotR = 7, reach = R - dotR;
	const px = (v: number) => C + v * reach;
	const py = (v: number) => C - v * reach;
	const innerR = (inner / 100) * reach;
	const outerR = reach - (outer / 100) * reach;
	const rawMag = Math.min(1, Math.hypot(x, y));
	const out = processStick(x, y, stick);          // instant, for the readout
	const outMag = Math.hypot(out.x, out.y);
	const angle = outMag > 0.08 ? ((Math.atan2(out.y, out.x) * 180 / Math.PI + 360) % 360) : null;
	const corPos = processStick(pos.x, pos.y, stick); // smoothed, for the dot

	// Configured output gate: min(unit square, ellipse 1+0.4142*d/100) —
	// same formula as the firmware's diagonal scaling (0 = circle, 100 = square)
	const ex = 1 + 0.4142 * diagX / 100;
	const ey = 1 + 0.4142 * diagY / 100;
	const gate = Array.from({ length: 72 }, (_, i) => {
		const a = (Math.PI / 36) * i;
		const ca = Math.cos(a), sa = Math.sin(a);
		const maxSquare = Math.min(1 / Math.max(Math.abs(ca), 1e-6), 1 / Math.max(Math.abs(sa), 1e-6));
		const maxEllipse = 1 / Math.sqrt(Math.max((ca * ca) / (ex * ex) + (sa * sa) / (ey * ey), 1e-9));
		const r = Math.min(maxSquare, maxEllipse) * reach;
		return `${(C + ca * r).toFixed(1)},${(C - sa * r).toFixed(1)}`;
	}).join(' ');
	const ticks = Array.from({ length: 24 }, (_, i) => {
		const a = (Math.PI / 12) * i;
		const r0 = R - (i % 6 === 0 ? 10 : 5);
		return { x1: C + Math.cos(a) * r0, y1: C + Math.sin(a) * r0, x2: C + Math.cos(a) * R, y2: C + Math.sin(a) * R };
	});
	const trailPath = trail.current.map((p, i) =>
		`${i === 0 ? 'M' : 'L'} ${px(p.x).toFixed(1)} ${py(p.y).toFixed(1)}`).join(' ');

	const W = 200, WH = 46;
	const wpts = wave.current.map((v, i) => `${(i / 119) * W},${WH - v * (WH - 4) - 2}`).join(' ');
	const gid = `glow-${label}`;

	return (
		<div style={{ textAlign: 'center' }}>
			<svg viewBox={`0 0 ${S} ${S}`} style={{ width: '100%', maxWidth: 260, display: 'block', margin: '0 auto' }}>
				<defs>
					<filter id={gid} x="-50%" y="-50%" width="200%" height="200%">
						<feGaussianBlur stdDeviation="2" result="b" />
						<feMerge><feMergeNode in="b" /><feMergeNode in="SourceGraphic" /></feMerge>
					</filter>
					<radialGradient id={`bg-${label}`} cx="50%" cy="42%" r="60%">
						<stop offset="0%" stopColor="rgba(47,214,255,0.06)" />
						<stop offset="100%" stopColor="rgba(0,0,0,0)" />
					</radialGradient>
				</defs>
				<circle cx={C} cy={C} r={R} fill={PANEL} stroke={LINE} strokeWidth="2" />
				<circle cx={C} cy={C} r={R} fill={`url(#bg-${label})`} />
				{ticks.map((t, i) => (
					<line key={i} x1={t.x1} y1={t.y1} x2={t.x2} y2={t.y2} stroke={LINE} strokeWidth="1" opacity="0.8" />
				))}
				<polygon points={gate} fill="none" stroke={ACCENT} strokeWidth="1.2" opacity="0.45" />
				{inner > 0 && <circle cx={C} cy={C} r={innerR} fill="none" stroke="#ff5c7a88" strokeWidth="1.4" strokeDasharray="3 3" />}
				{outer < 100 && <circle cx={C} cy={C} r={outerR} fill="none" stroke="#ff5c7a55" strokeWidth="1.4" strokeDasharray="3 3" />}
				<line x1={C - R} y1={C} x2={C + R} y2={C} stroke={LINE} strokeWidth="1" opacity="0.45" />
				<line x1={C} y1={C - R} x2={C} y2={C + R} stroke={LINE} strokeWidth="1" opacity="0.45" />
				<path d={trailPath} fill="none" stroke={ACCENT} strokeWidth="2.5" opacity="0.3" strokeLinecap="round" strokeLinejoin="round" />
				<line x1={C} y1={C} x2={px(corPos.x)} y2={py(corPos.y)} stroke={ACCENT} strokeWidth="2.5" opacity="0.55" />
				<line x1={px(pos.x)} y1={py(pos.y)} x2={px(corPos.x)} y2={py(corPos.y)}
					stroke={INK} strokeWidth="1" strokeDasharray="2 3" opacity="0.5" />
				<circle cx={px(pos.x)} cy={py(pos.y)} r={4}
					fill="none" stroke="#fff" strokeWidth="1.5" opacity="0.65" />
				<circle cx={px(corPos.x)} cy={py(corPos.y)} r={dotR} filter={`url(#${gid})`}
					fill={pressed ? ON : ACCENT} stroke={pressed ? ON : '#fff'}
					strokeWidth={pressed ? 3 : 1.5} opacity="0.96" />
				<text x={C} y={S - 6} textAnchor="middle" fontSize="13" fontWeight="700" fill={INK}>{label}</text>
			</svg>
			<svg viewBox={`0 0 ${W} ${WH}`} style={{ width: '100%', maxWidth: 260, height: 40, display: 'block', margin: '2px auto 0' }}>
				<rect x="0" y="0" width={W} height={WH} fill={PANEL} stroke={LINE} strokeWidth="1" rx="4" />
				<polyline points={wpts} fill="none" stroke={ACCENT2} strokeWidth="1.6" opacity="0.9" />
			</svg>
			<div className="small mt-1" style={{ fontVariantNumeric: 'tabular-nums', color: INK }}>
				<div className="text-muted">入力 X {fmt(x)} · Y {fmt(y)} · {Math.round(rawMag * 100)}%</div>
				<div>
					出力 X {fmt(out.x)} · Y {fmt(out.y)}
					<span className="text-muted"> · {Math.round(outMag * 100)}%{angle !== null ? ` · ${Math.round(angle)}°` : ''}</span>
					{pressed && <span style={{ color: ON }}> · {label}3</span>}
				</div>
			</div>
		</div>
	);
}

function TriggerMeter({ label, value, raw }: { label: string; value: number; raw: number }) {
	return (
		<div style={{ textAlign: 'center' }}>
			<div className="small mb-1" style={{ fontWeight: 700, color: INK }}>{label}</div>
			<div style={{
				width: 36, height: 180, margin: '0 auto',
				background: PANEL, border: `1px solid ${LINE}`,
				borderRadius: 8, position: 'relative', overflow: 'hidden',
			}}>
				<div style={{
					position: 'absolute', left: 0, right: 0, bottom: 0,
					height: `${value * 100}%`,
					background: `linear-gradient(to top, ${ACCENT}, ${ACCENT2})`,
					transition: 'height 60ms linear',
				}} />
			</div>
			<div className="small mt-1" style={{ fontVariantNumeric: 'tabular-nums', color: INK }}>{Math.round(value * 100)}%</div>
			<div className="text-muted" style={{ fontSize: 10 }}>{raw}</div>
		</div>
	);
}

function Btn({ label, active }: { label: string; active: boolean }) {
	return (
		<span style={{
			display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
			boxSizing: 'border-box', width: 56, height: 42, padding: 0, margin: 4, borderRadius: 8,
			fontSize: 17, fontWeight: 700, lineHeight: 1, overflow: 'hidden', whiteSpace: 'nowrap',
			background: active ? ON : PANEL,
			color: active ? '#1a1200' : 'var(--bs-secondary-color, #8b949e)',
			border: `1px solid ${active ? ON : LINE}`,
			boxShadow: active ? `0 0 10px ${ON}66` : 'none',
			transition: 'background 40ms, color 40ms, box-shadow 40ms',
		}}>{label}</span>
	);
}

export default function GamepadTester({ raw, opts }: { raw: RawData | null; opts: Opts }) {
	if (!raw || !raw.valid) return null;
	const g = raw.gpio;
	const on = (name: string) => {
		const pin = Object.keys(PIN_BUTTONS).find((p) => PIN_BUTTONS[+p] === name);
		return pin !== undefined && pinDown(g, +pin);
	};

	const analogBtn = (ch: number | undefined, thr: number | undefined, hi: boolean | undefined) => {
		if (ch === undefined || ch < 0) return false;
		const v = raw.channels[ch] ?? 0;
		return hi ? v >= (thr ?? 32768) : v <= (thr ?? 32768);
	};
	const l3 = pinDown(g, opts.l3Pin) || analogBtn(opts.chL3, opts.l3Threshold, opts.l3ActiveHigh);
	const r3 = pinDown(g, opts.r3Pin) || analogBtn(opts.chR3, opts.r3Threshold, opts.r3ActiveHigh);
	const psRaw = opts.chPsButton >= 0 ? raw.channels[opts.chPsButton] : 65535;
	const ps = opts.chPsButton >= 0 && (opts.psActiveHigh ? psRaw >= opts.psThreshold : psRaw <= opts.psThreshold);

	const ls = opts.leftStick, rs = opts.rightStick;
	// X wiring is reversed vs console convention (firmware negates rawX), so
	// negate here too; "X軸反転" then flips on top of that
	const lx = -calAxis(raw.channels[opts.chLX] ?? 32768, { mn: ls.calMinX, ctr: ls.calCenterX, mx: ls.calMaxX }, !!ls.invertX);
	// screen +y = up, so negate the raw axis (raw grows downward)
	const ly = -calAxis(raw.channels[opts.chLY] ?? 32768, { mn: ls.calMinY, ctr: ls.calCenterY, mx: ls.calMaxY }, !!ls.invertY);
	const rx = -calAxis(raw.channels[opts.chRX] ?? 32768, { mn: rs.calMinX, ctr: rs.calCenterX, mx: rs.calMaxX }, !!rs.invertX);
	const ry = -calAxis(raw.channels[opts.chRY] ?? 32768, { mn: rs.calMinY, ctr: rs.calCenterY, mx: rs.calMaxY }, !!rs.invertY);
	const lt = trigTravel(raw.adc[0] ?? LT.idle, LT);
	const rt = trigTravel(raw.adc[1] ?? RT.idle, RT);

	return (
		<div style={{ padding: '4px 2px' }}>
			<div className="d-flex flex-wrap justify-content-center align-items-center gap-4">
				<TriggerMeter label="L2" value={lt} raw={raw.adc[0] ?? 0} />
				<StickScope label="L" x={lx} y={ly} pressed={l3} stick={ls} />
				<StickScope label="R" x={rx} y={ry} pressed={r3} stick={rs} />
				<TriggerMeter label="R2" value={rt} raw={raw.adc[1] ?? 0} />
			</div>
			<div className="text-center mt-3">
				<div>
					<Btn label="L1" active={on('l1')} />
					<Btn label="R1" active={on('r1')} />
					<Btn label="□" active={on('b3')} />
					<Btn label="△" active={on('b4')} />
					<Btn label="✕" active={on('b1')} />
					<Btn label="○" active={on('b2')} />
				</div>
				<div className="mt-1">
					<Btn label="↑" active={on('up')} />
					<Btn label="↓" active={on('down')} />
					<Btn label="←" active={on('left')} />
					<Btn label="→" active={on('right')} />
					<Btn label="SH" active={on('s1')} />
					<Btn label="OP" active={on('s2')} />
					<Btn label="TP" active={on('tp')} />
					<Btn label="PS" active={ps} />
				</div>
			</div>
		</div>
	);
}
