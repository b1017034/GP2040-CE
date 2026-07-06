// Visual live tester for the HML board: shows which physical control is
// active, gamepad-tester style. Drawn as a DS4-ish SVG diagram.
import { } from 'react';

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
	leftStick: any; rightStick: any;
};

// pin -> label (active low)
const PIN_BUTTONS: Record<number, string> = {
	8: 'down', 16: 'left', 17: 'right', 23: 'up',
	15: 'b1', 9: 'b2', 13: 'b3', 14: 'b4',
	22: 'l1', 21: 'r1', 18: 's1', 19: 's2', 12: 'tp',
};

// trigger calibration (matches firmware BoardConfig)
const LT = { idle: 4060, pressed: 20 };
const RT = { idle: 2057, pressed: 20 };

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
	v = Math.max(-1, Math.min(1, v));
	return invert ? -v : v;
}

function trigTravel(raw: number, c: { idle: number; pressed: number }): number {
	const t = (c.idle - raw) / (c.idle - c.pressed);
	return Math.max(0, Math.min(1, t));
}

const ON = '#ffc107';
const OFF = 'var(--bs-secondary-bg)';
const STROKE = '#888';

export default function GamepadTester({ raw, opts }: { raw: RawData | null; opts: Opts }) {
	if (!raw || !raw.valid) return null;
	const g = raw.gpio;
	const on = (name: string) => {
		const pin = Object.keys(PIN_BUTTONS).find((p) => PIN_BUTTONS[+p] === name);
		return pin !== undefined && pinDown(g, +pin);
	};
	const l3 = pinDown(g, opts.l3Pin);
	const r3 = pinDown(g, opts.r3Pin);
	const psRaw = opts.chPsButton >= 0 ? raw.channels[opts.chPsButton] : 65535;
	const ps = opts.chPsButton >= 0 && (opts.psActiveHigh ? psRaw >= opts.psThreshold : psRaw <= opts.psThreshold);

	const ls = opts.leftStick, rs = opts.rightStick;
	const lx = calAxis(raw.channels[opts.chLX] ?? 32768, { mn: ls.calMinX, ctr: ls.calCenterX, mx: ls.calMaxX }, !!ls.invertX);
	const ly = calAxis(raw.channels[opts.chLY] ?? 32768, { mn: ls.calMinY, ctr: ls.calCenterY, mx: ls.calMaxY }, !!ls.invertY);
	const rx = calAxis(raw.channels[opts.chRX] ?? 32768, { mn: rs.calMinX, ctr: rs.calCenterX, mx: rs.calMaxX }, !!rs.invertX);
	const ry = calAxis(raw.channels[opts.chRY] ?? 32768, { mn: rs.calMinY, ctr: rs.calCenterY, mx: rs.calMaxY }, !!rs.invertY);
	const lt = trigTravel(raw.adc[0] ?? LT.idle, LT);
	const rt = trigTravel(raw.adc[1] ?? RT.idle, RT);

	const btn = (active: boolean) => ({ fill: active ? ON : OFF, stroke: STROKE, strokeWidth: 1.5 });

	return (
		<svg viewBox="0 0 460 240" style={{ width: '100%', maxWidth: 560 }}>
			{/* body */}
			<path d="M60 60 Q90 35 150 40 L310 40 Q370 35 400 60 Q440 90 435 150 Q430 205 395 210 Q360 214 340 180 L320 155 L140 155 L120 180 Q100 214 65 210 Q30 205 25 150 Q20 90 60 60 Z"
				fill="none" stroke={STROKE} strokeWidth="2" />
			{/* triggers */}
			<rect x={95} y={6} width={18} height={26} rx={3} fill="none" stroke={STROKE} />
			<rect x={95} y={6 + 26 * (1 - lt)} width={18} height={26 * lt} rx={2} fill={ON} />
			<text x={104} y={46} textAnchor="middle" fontSize="9" fill="currentColor">L2</text>
			<rect x={347} y={6} width={18} height={26} rx={3} fill="none" stroke={STROKE} />
			<rect x={347} y={6 + 26 * (1 - rt)} width={18} height={26 * rt} rx={2} fill={ON} />
			<text x={356} y={46} textAnchor="middle" fontSize="9" fill="currentColor">R2</text>
			{/* bumpers */}
			<rect x={125} y={24} width={44} height={12} rx={5} {...btn(on('l1'))} />
			<text x={147} y={21} textAnchor="middle" fontSize="9" fill="currentColor">L1</text>
			<rect x={291} y={24} width={44} height={12} rx={5} {...btn(on('r1'))} />
			<text x={313} y={21} textAnchor="middle" fontSize="9" fill="currentColor">R1</text>
			{/* dpad */}
			<rect x={78} y={78} width={22} height={22} {...btn(on('up'))} />
			<rect x={78} y={122} width={22} height={22} {...btn(on('down'))} />
			<rect x={56} y={100} width={22} height={22} {...btn(on('left'))} />
			<rect x={100} y={100} width={22} height={22} {...btn(on('right'))} />
			<text x={89} y={73} textAnchor="middle" fontSize="9" fill="currentColor">十字</text>
			{/* face buttons */}
			<circle cx={371} cy={78} r={12} {...btn(on('b4'))} />
			<text x={371} y={82} textAnchor="middle" fontSize="10">△</text>
			<circle cx={399} cy={106} r={12} {...btn(on('b2'))} />
			<text x={399} y={110} textAnchor="middle" fontSize="10">○</text>
			<circle cx={343} cy={106} r={12} {...btn(on('b3'))} />
			<text x={343} y={110} textAnchor="middle" fontSize="10">□</text>
			<circle cx={371} cy={134} r={12} {...btn(on('b1'))} />
			<text x={371} y={138} textAnchor="middle" fontSize="10">✕</text>
			{/* touchpad */}
			<rect x={185} y={55} width={90} height={40} rx={6} {...btn(on('tp'))} />
			<text x={230} y={78} textAnchor="middle" fontSize="9" fill="currentColor">タッチパッド</text>
			{/* share/options */}
			<rect x={163} y={58} width={12} height={22} rx={4} {...btn(on('s1'))} />
			<text x={169} y={92} textAnchor="middle" fontSize="8" fill="currentColor">SH</text>
			<rect x={285} y={58} width={12} height={22} rx={4} {...btn(on('s2'))} />
			<text x={291} y={92} textAnchor="middle" fontSize="8" fill="currentColor">OP</text>
			{/* PS */}
			<circle cx={230} cy={140} r={10} {...btn(ps)} />
			<text x={230} y={158} textAnchor="middle" fontSize="8" fill="currentColor">PS</text>
			{/* sticks */}
			<circle cx={160} cy={135} r={26} fill="none" stroke={STROKE} />
			<circle cx={160 + lx * 16} cy={135 + -ly * 16} r={11} {...btn(l3)} />
			<text x={160} y={172} textAnchor="middle" fontSize="8" fill="currentColor">
				L {lx.toFixed(2)}, {ly.toFixed(2)}{l3 ? ' [L3]' : ''}
			</text>
			<circle cx={300} cy={135} r={26} fill="none" stroke={STROKE} />
			<circle cx={300 + rx * 16} cy={135 + -ry * 16} r={11} {...btn(r3)} />
			<text x={300} y={172} textAnchor="middle" fontSize="8" fill="currentColor">
				R {rx.toFixed(2)}, {ry.toFixed(2)}{r3 ? ' [R3]' : ''}
			</text>
		</svg>
	);
}
