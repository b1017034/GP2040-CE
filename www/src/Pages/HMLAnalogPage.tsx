import { useEffect, useRef, useState, useCallback } from 'react';
import {
	Alert,
	Button,
	Card,
	Col,
	Form,
	Row,
	Tab,
	Tabs,
} from 'react-bootstrap';
import Http from '../Services/Http';
import GamepadTester, { applyCurve } from '../Components/GamepadTester';
import { baseUrl } from '../Services/WebApi';

const CURVE_PRESETS: Record<number, { label: string; pts: number[][] }> = {
	0: { label: 'リニア (Linear)', pts: [[30, 30], [70, 70], [100, 100]] },
	1: { label: 'スロースタート (Slow start)', pts: [[60, 30], [90, 80], [100, 100]] },
	2: { label: 'クイックスタート (Quick start)', pts: [[30, 60], [70, 90], [100, 100]] },
	3: { label: 'インスタント (Instant)', pts: [[20, 80], [60, 95], [100, 100]] },
	4: { label: 'スムーズスタート (Smooth start)', pts: [[30, 25], [70, 75], [100, 100]] },
	5: { label: 'カスタム (Custom)', pts: [] },
};

type StickOpts = {
	calMinX: number; calCenterX: number; calMaxX: number;
	calMinY: number; calCenterY: number; calMaxY: number;
	innerDeadzone: number; outerDeadzone: number;
	curvePreset: number; curvePointCount: number;
	curveX: number[]; curveY: number[];
	rcFilterStrength: number; rcSnapback: number; rcAdvanced: boolean;
	rcCurvePointCount: number; rcCurveX: number[]; rcCurveY: number[];
	rcVelocityMult100: number;
	invertX: boolean; invertY: boolean;
	diagScaleX: number; diagScaleY: number;
	noiseSuppress: boolean; noiseX: number; noiseY: number;
	// FireBird 基本設定
	scaleUp: number; scaleDown: number; scaleLeft: number; scaleRight: number;
	innerX: number; innerY: number; outerX: number; outerY: number;
	antiX: number; antiY: number;
	// FireBird 詳細設定 (output correction)
	resolution: number; delayMs: number;
	// FireBird 角度補正
	angleOffset100: number; angleSnap: number;
	angleMapCount: number; angleMapEnable: number;
	angleMapIn100: number[]; angleMapOut100: number[]; angleMapDz100: number[];
};

type Options = {
	enabled: number | boolean;
	chLX: number; chLY: number; chRX: number; chRY: number;
	chPsButton: number; psThreshold: number; psActiveHigh: boolean;
	ltAdcPin: number; rtAdcPin: number;
	l3Pin: number; r3Pin: number;
	chL3?: number; l3Threshold?: number; l3ActiveHigh?: boolean;
	chR3?: number; r3Threshold?: number; r3ActiveHigh?: boolean;
	usbHostEnabled: boolean; usbHostDp: number;
	lightbarEnabled: boolean; lightbarDataPin: number; lightbarBrightness: number; lightbarColor: number;
	leftStick: StickOpts; rightStick: StickOpts;
};

type RawData = {
	channels: number[];
	adc: number[];
	gpio?: number;
	valid: boolean;
	lx: number; ly: number; rx: number; ry: number;
	lt: number; rt: number;
};

const defaultStick = (): StickOpts => ({
	calMinX: 0, calCenterX: 0, calMaxX: 0,
	calMinY: 0, calCenterY: 0, calMaxY: 0,
	innerDeadzone: 5, outerDeadzone: 5,
	curvePreset: 0, curvePointCount: 3,
	curveX: [30, 70, 100], curveY: [30, 70, 100],
	rcFilterStrength: 0, rcSnapback: 0, rcAdvanced: false,
	rcCurvePointCount: 2, rcCurveX: [0, 128], rcCurveY: [0, 0],
	rcVelocityMult100: 100,
	invertX: false, invertY: false,
	diagScaleX: 0, diagScaleY: 0,
	noiseSuppress: false, noiseX: 0, noiseY: 0,
	scaleUp: 100, scaleDown: 100, scaleLeft: 100, scaleRight: 100,
	innerX: 0, innerY: 0, outerX: 100, outerY: 100,
	antiX: 0, antiY: 0,
	resolution: 0, delayMs: 0,
	angleOffset100: 0, angleSnap: 0,
	angleMapCount: 0, angleMapEnable: 0,
	angleMapIn100: [], angleMapOut100: [], angleMapDz100: [],
});

// Graph frame: 200x200 plot with grid, diagonal reference, axis labels.
// `plot` is a function 0..1 -> 0..1 (input -> output), sampled to a path.
function GraphSvg({
	plot, markers, color = '#ff8c1a',
}: {
	plot: (t: number) => number;
	markers?: { x: number; y: number; c?: string }[];
	color?: string;
}) {
	const N = 64;
	const pts: string[] = [];
	for (let i = 0; i <= N; i++) {
		const t = i / N;
		const v = Math.max(0, Math.min(1, plot(t)));
		pts.push(`${(t * 200).toFixed(1)},${(200 - v * 200).toFixed(1)}`);
	}
	const line = 'M ' + pts.join(' L ');
	const area = `M 0,200 L ${pts.join(' L ')} L 200,200 Z`;
	const grid = [0.25, 0.5, 0.75];
	return (
		<svg viewBox="-16 -10 232 232" style={{ width: 240, maxWidth: '100%', background: 'var(--bs-tertiary-bg)', borderRadius: 6 }}>
			<rect x="0" y="0" width="200" height="200" fill="none" stroke="#888" strokeWidth="1" />
			{grid.map((g) => (
				<g key={g}>
					<line x1={g * 200} y1="0" x2={g * 200} y2="200" stroke="#8884" strokeWidth="1" />
					<line x1="0" y1={g * 200} x2="200" y2={g * 200} stroke="#8884" strokeWidth="1" />
				</g>
			))}
			<line x1="0" y1="200" x2="200" y2="0" stroke="#666" strokeDasharray="4" />
			<path d={area} fill={color} fillOpacity="0.18" stroke="none" />
			<path d={line} fill="none" stroke={color} strokeWidth="3" />
			{(markers ?? []).map((m, i) => (
				<circle key={i} cx={m.x * 200} cy={200 - m.y * 200} r="5"
					fill={m.c ?? '#ffc107'} stroke="#222" strokeWidth="1" />
			))}
			<text x="100" y="224" fill="#999" fontSize="12" textAnchor="middle">入力 →</text>
			<text x="-13" y="100" fill="#999" fontSize="12" textAnchor="middle"
				transform="rotate(-90 -13 100)">↑ 出力</text>
		</svg>
	);
}

// Output-shape preview: circle -> square morph gate min(square, ellipse)
function ShapeSvg({ diagX, diagY }: { diagX: number; diagY: number }) {
	const ex = 1 + 0.4142 * diagX / 100;
	const ey = 1 + 0.4142 * diagY / 100;
	const C = 100, R = 90;
	const pts: string[] = [];
	for (let i = 0; i <= 96; i++) {
		const a = (Math.PI * 2 / 96) * i;
		const ca = Math.cos(a), sa = Math.sin(a);
		const maxSquare = Math.min(1 / Math.max(Math.abs(ca), 1e-6), 1 / Math.max(Math.abs(sa), 1e-6));
		const maxEllipse = 1 / Math.sqrt(Math.max((ca * ca) / (ex * ex) + (sa * sa) / (ey * ey), 1e-9));
		const r = Math.min(maxSquare, maxEllipse) * R;
		pts.push(`${(C + ca * r).toFixed(1)},${(C - sa * r).toFixed(1)}`);
	}
	return (
		<svg viewBox="0 0 200 200" style={{ width: 160, maxWidth: '100%', background: 'var(--bs-tertiary-bg)', borderRadius: 6 }}>
			<rect x={C - R} y={C - R} width={R * 2} height={R * 2} fill="none" stroke="#555" strokeDasharray="4" />
			<circle cx={C} cy={C} r={R} fill="none" stroke="#555" strokeDasharray="4" />
			<line x1={C} y1="6" x2={C} y2="194" stroke="#8884" />
			<line x1="6" y1={C} x2="194" y2={C} stroke="#8884" />
			<polygon points={pts.join(' ')} fill="#22c55e" fillOpacity="0.18" stroke="#22c55e" strokeWidth="2.5" />
		</svg>
	);
}

// RC filter curve preview: velocity 0..128 -> strength -500..500
function RcCurveSvg({ stick }: { stick: StickOpts }) {
	const n = Math.max(2, stick.rcCurvePointCount || 2);
	const xs = stick.rcCurveX.slice(0, n);
	const ys = stick.rcCurveY.slice(0, n);
	const lerp = (vx: number) => {
		let px = 0, py = ys[0] ?? 0;
		for (let i = 0; i < n; i++) {
			const nx = i === 0 ? 0 : i === n - 1 ? 128 : (xs[i] ?? 64);
			const ny = ys[i] ?? 0;
			if (vx <= nx) { const d = nx - px; return d <= 0.001 ? ny : py + (vx - px) * (ny - py) / d; }
			px = nx; py = ny;
		}
		return py;
	};
	const pts: string[] = [];
	for (let i = 0; i <= 64; i++) {
		const vx = (128 / 64) * i;
		const v = lerp(vx);
		pts.push(`${(vx / 128 * 200).toFixed(1)},${(100 - v / 500 * 100).toFixed(1)}`);
	}
	return (
		<svg viewBox="-6 -6 212 212" style={{ width: 220, maxWidth: '100%', background: 'var(--bs-tertiary-bg)', borderRadius: 6 }}>
			<rect x="0" y="0" width="200" height="200" fill="none" stroke="#888" />
			<line x1="0" y1="100" x2="200" y2="100" stroke="#8886" />
			<path d={'M ' + pts.join(' L ')} fill="none" stroke="#ff8c1a" strokeWidth="2.5" />
			<text x="4" y="14" fill="#999" fontSize="11">+500</text>
			<text x="4" y="196" fill="#999" fontSize="11">-500</text>
			<text x="150" y="96" fill="#999" fontSize="11">速度128</text>
		</svg>
	);
}

function CurveEditor({
	stick, onChange,
}: { stick: StickOpts; onChange: (s: StickOpts) => void }) {
	const isCustom = stick.curvePreset >= 5;
	const count = isCustom
		? Math.min(stick.curvePointCount || stick.curveX.length, stick.curveX.length)
		: 3;
	const markers = isCustom
		? Array.from({ length: count }, (_, i) => ({ x: (stick.curveX[i] ?? 100) / 100, y: (stick.curveY[i] ?? 100) / 100 }))
		: (CURVE_PRESETS[stick.curvePreset]?.pts ?? CURVE_PRESETS[0].pts).map(([x, y]) => ({ x: x / 100, y: y / 100 }));

	return (
		<div>
			<Form.Group className="mb-2">
				<Form.Label>感度カーブプリセット</Form.Label>
				<Form.Select
					value={stick.curvePreset}
					onChange={(e) => onChange({ ...stick, curvePreset: +e.target.value })}
				>
					{Object.entries(CURVE_PRESETS).map(([v, p]) => (
						<option key={v} value={v}>{p.label}</option>
					))}
				</Form.Select>
			</Form.Group>
			<GraphSvg plot={(t) => applyCurve(t, stick)} markers={markers} />
			{isCustom && (
				<>
					<Form.Group className="mb-2 mt-2">
						<Form.Label>ポイント数: {stick.curvePointCount}</Form.Label>
						<Form.Range min={1} max={10} value={stick.curvePointCount}
							onChange={(e) => {
								const n = +e.target.value;
								const cx = [...stick.curveX], cy = [...stick.curveY];
								while (cx.length < n) { cx.push(100); cy.push(100); }
								onChange({ ...stick, curvePointCount: n, curveX: cx.slice(0, Math.max(n, cx.length)), curveY: cy.slice(0, Math.max(n, cy.length)) });
							}} />
					</Form.Group>
					{Array.from({ length: stick.curvePointCount }).map((_, i) => (
						<Row key={i} className="mb-1">
							<Col xs={2}>P{i + 1}</Col>
							<Col>
								<Form.Label className="small">入力 {stick.curveX[i] ?? 100}</Form.Label>
								<Form.Range min={0} max={100} value={stick.curveX[i] ?? 100}
									onChange={(e) => {
										const cx = [...stick.curveX]; cx[i] = +e.target.value;
										onChange({ ...stick, curveX: cx });
									}} />
							</Col>
							<Col>
								<Form.Label className="small">出力 {stick.curveY[i] ?? 100}</Form.Label>
								<Form.Range min={0} max={100} value={stick.curveY[i] ?? 100}
									onChange={(e) => {
										const cy = [...stick.curveY]; cy[i] = +e.target.value;
										onChange({ ...stick, curveY: cy });
									}} />
							</Col>
						</Row>
					))}
				</>
			)}
		</div>
	);
}

// Small labelled slider + numeric box pair (FireBird control style)
function NumRange({
	label, value, min, max, step, suffix, onChange,
}: {
	label: string; value: number; min: number; max: number;
	step?: number; suffix?: string; onChange: (v: number) => void;
}) {
	return (
		<Form.Group className="mb-2">
			<div className="d-flex justify-content-between">
				<Form.Label className="mb-1">{label}</Form.Label>
				<Form.Control type="number" size="sm" style={{ width: 90 }}
					min={min} max={max} step={step ?? 1} value={value}
					onChange={(e) => onChange(Number(e.target.value))} />
			</div>
			<Form.Range min={min} max={max} step={step ?? 1} value={value}
				onChange={(e) => onChange(+e.target.value)} />
			{suffix && <div className="text-muted small">{suffix}</div>}
		</Form.Group>
	);
}

// フィルター tab: RC filter (simple/advanced), snapback, noise stabilization
function FilterTab({
	stick, chX, chY, onChange,
}: { stick: StickOpts; chX: number; chY: number; onChange: (s: StickOpts) => void }) {
	const [noiseMeasuring, setNoiseMeasuring] = useState(false);
	const measureNoise = async () => {
		setNoiseMeasuring(true);
		try {
			const { data } = await Http.get(`${baseUrl}/api/getHMLNoiseSample`);
			if (data?.valid) {
				const p2p: number[] = data.p2p ?? [];
				onChange({
					...stick,
					noiseX: chX >= 0 ? (p2p[chX] ?? 0) : 0,
					noiseY: chY >= 0 ? (p2p[chY] ?? 0) : 0,
				});
			}
		} catch (e) { console.error(e); }
		setNoiseMeasuring(false);
	};
	const noiseUnmeasured = (stick.noiseX ?? 0) === 0 && (stick.noiseY ?? 0) === 0;
	// FireBird RC curve: index 0 = 静止(x=0), last = 最大速度(x=128)
	const rcPts = Math.max(2, stick.rcCurvePointCount || 2);
	const rcLabel = (i: number) =>
		i === 0 ? '静止 (0)' : i === rcPts - 1 ? `最大速度 (128)` : `P${i}`;

	return (
		<div>
			<Card className="mb-3">
				<Card.Header>RCフィルター</Card.Header>
				<Card.Body>
					<p className="small text-muted">
						＋方向: ジッター減 ／ −方向: 先読み（FireBird v3.08 実機と同一式。+500 でも遅延は約5msで体感不能）
					</p>
					<Tabs
						activeKey={stick.rcAdvanced ? 'adv' : 'simple'}
						onSelect={(k) => onChange({ ...stick, rcAdvanced: k === 'adv' })}
						className="mb-3">
						<Tab eventKey="simple" title="簡易設定">
							<NumRange label="フィルター強度" min={-500} max={500}
								value={stick.rcFilterStrength}
								onChange={(v) => onChange({ ...stick, rcFilterStrength: v })} />
						</Tab>
						<Tab eventKey="adv" title="詳細設定">
							<div className="text-center mb-2"><RcCurveSvg stick={stick} /></div>
							<NumRange label="速度の倍率" min={0} max={1000}
								value={stick.rcVelocityMult100}
								suffix={`×${(stick.rcVelocityMult100 / 100).toFixed(2)}`}
								onChange={(v) => onChange({ ...stick, rcVelocityMult100: v })} />
							<Form.Group className="mb-2">
								<Form.Label>マーカーの数</Form.Label>
								<Form.Select value={rcPts}
									onChange={(e) => {
										const n = +e.target.value;
										const cx = [...stick.rcCurveX], cy = [...stick.rcCurveY];
										while (cx.length < n) { cx.push(64); cy.push(0); }
										cx[0] = 0; cx[n - 1] = 128;
										onChange({ ...stick, rcCurvePointCount: n, rcCurveX: cx.slice(0, n), rcCurveY: cy.slice(0, n) });
									}}>
									{[2, 3, 4, 5, 6, 7].map((n) => <option key={n} value={n}>{n}</option>)}
								</Form.Select>
							</Form.Group>
							<Form.Label className="small">速度(0〜128)→強度カーブ</Form.Label>
							{Array.from({ length: rcPts }).map((_, i) => (
								<Row key={i} className="mb-1 align-items-end">
									<Col xs={3} className="small">{rcLabel(i)}</Col>
									{i !== 0 && i !== rcPts - 1 ? (
										<Col>
											<Form.Label className="small mb-0">速度 {stick.rcCurveX[i] ?? 64}</Form.Label>
											<Form.Range min={0} max={128} value={stick.rcCurveX[i] ?? 64}
												onChange={(e) => {
													const v = [...stick.rcCurveX]; v[i] = +e.target.value;
													onChange({ ...stick, rcCurveX: v });
												}} />
										</Col>
									) : <Col />}
									<Col>
										<Form.Label className="small mb-0">強度 {stick.rcCurveY[i] ?? 0}</Form.Label>
										<Form.Range min={-500} max={500} value={stick.rcCurveY[i] ?? 0}
											onChange={(e) => {
												const v = [...stick.rcCurveY]; v[i] = +e.target.value;
												onChange({ ...stick, rcCurveY: v });
											}} />
									</Col>
								</Row>
							))}
						</Tab>
					</Tabs>
					<hr />
					<h6>動作の安定化</h6>
					<Form.Check type="switch" label="安定化を有効にする"
						checked={!!stick.noiseSuppress}
						disabled={noiseUnmeasured}
						onChange={(e) => onChange({ ...stick, noiseSuppress: e.target.checked })} />
					<div className="text-muted small">
						{noiseUnmeasured
							? 'ノイズ未計測のため使用できません。下のボタンで計測してください。'
							: `計測ノイズ幅: X ${stick.noiseX} / Y ${stick.noiseY} (16bit値)。静止時のジッターを完全に停止します。`}
					</div>
					<Button size="sm" variant="outline-secondary" className="mt-1"
						disabled={noiseMeasuring} onClick={measureNoise}>
						{noiseMeasuring ? '計測中…' : 'ノイズ計測(スティックに触れずに実行)'}
					</Button>
				</Card.Body>
			</Card>

			<Card className="mb-3">
				<Card.Header>スナップバックフィルター</Card.Header>
				<Card.Body>
					<NumRange label="フィルター強度" min={0} max={100} value={stick.rcSnapback}
						suffix="中心へ戻る動きだけを平滑化します（0 = 無効）。"
						onChange={(v) => onChange({ ...stick, rcSnapback: v })} />
				</Card.Body>
			</Card>
		</div>
	);
}

// 角度補正 tab: global offset, angle snap, base angle map
function AngleTab({
	stick, onChange,
}: { stick: StickOpts; onChange: (s: StickOpts) => void }) {
	const rows = stick.angleMapCount || 0;
	const setCount = (n: number) => {
		const mkIn = [...stick.angleMapIn100];
		const mkOut = [...stick.angleMapOut100];
		const mkDz = [...stick.angleMapDz100];
		let en = stick.angleMapEnable;
		while (mkIn.length < n) {
			const deg = mkIn.length * 45; // seed at 45° spacing like FireBird
			mkIn.push(deg * 100); mkOut.push(deg * 100); mkDz.push(0);
			en |= (1 << (mkIn.length - 1));
		}
		onChange({
			...stick, angleMapCount: n,
			angleMapIn100: mkIn.slice(0, n), angleMapOut100: mkOut.slice(0, n),
			angleMapDz100: mkDz.slice(0, n), angleMapEnable: en,
		});
	};
	return (
		<Card className="mb-3">
			<Card.Header>角度補正</Card.Header>
			<Card.Body>
				<NumRange label="全体角度オフセット (°)" min={-180} max={180} step={0.5}
					value={stick.angleOffset100 / 100}
					onChange={(v) => onChange({ ...stick, angleOffset100: Math.round(v * 100) })} />
				<Form.Group className="mb-3">
					<Form.Label>角度固定</Form.Label>
					<Form.Select value={stick.angleSnap}
						onChange={(e) => onChange({ ...stick, angleSnap: +e.target.value })}>
						<option value={0}>オフ</option>
						<option value={1}>45°</option>
						<option value={2}>90°</option>
					</Form.Select>
				</Form.Group>
				<hr />
				<Form.Group className="mb-2">
					<Form.Label>基準角度マップ数</Form.Label>
					<Form.Select value={rows} onChange={(e) => setCount(+e.target.value)}>
						{Array.from({ length: 17 }).map((_, n) => <option key={n} value={n}>{n}</option>)}
					</Form.Select>
					<div className="text-muted small">入力角度→出力角度に補正。デッドゾーンは各点で出力を固定する幅。</div>
				</Form.Group>
				{rows > 0 && (
					<Row className="small fw-bold mb-1">
						<Col xs={1}>#</Col><Col>入力角度</Col><Col>出力角度</Col>
						<Col>デッドゾーン</Col><Col xs={2}>有効</Col>
					</Row>
				)}
				{Array.from({ length: rows }).map((_, i) => (
					<Row key={i} className="mb-1 align-items-center">
						<Col xs={1}>{i + 1}</Col>
						<Col>
							<Form.Control type="number" size="sm" min={0} max={360} step={0.5}
								value={(stick.angleMapIn100[i] ?? 0) / 100}
								onChange={(e) => {
									const v = [...stick.angleMapIn100]; v[i] = Math.round(Number(e.target.value) * 100);
									onChange({ ...stick, angleMapIn100: v });
								}} />
						</Col>
						<Col>
							<Form.Control type="number" size="sm" min={0} max={360} step={0.5}
								value={(stick.angleMapOut100[i] ?? 0) / 100}
								onChange={(e) => {
									const v = [...stick.angleMapOut100]; v[i] = Math.round(Number(e.target.value) * 100);
									onChange({ ...stick, angleMapOut100: v });
								}} />
						</Col>
						<Col>
							<Form.Control type="number" size="sm" min={0} max={45} step={0.5}
								value={(stick.angleMapDz100[i] ?? 0) / 100}
								onChange={(e) => {
									const v = [...stick.angleMapDz100]; v[i] = Math.round(Number(e.target.value) * 100);
									onChange({ ...stick, angleMapDz100: v });
								}} />
						</Col>
						<Col xs={2}>
							<Form.Check type="switch"
								checked={(stick.angleMapEnable & (1 << i)) !== 0}
								onChange={(e) => {
									const en = e.target.checked
										? stick.angleMapEnable | (1 << i)
										: stick.angleMapEnable & ~(1 << i);
									onChange({ ...stick, angleMapEnable: en });
								}} />
						</Col>
					</Row>
				))}
			</Card.Body>
		</Card>
	);
}

// 基本設定 tab: calibration, input scale, scaling shape, per-axis deadzones
function BasicTab({
	stick, chX, chY, raw, onChange,
}: {
	stick: StickOpts; chX: number; chY: number;
	raw: RawData | null; onChange: (s: StickOpts) => void;
}) {
	const [calStage, setCalStage] = useState<'idle' | 'center' | 'range'>('idle');
	const calData = useRef({ minX: 65535, maxX: 0, minY: 65535, maxY: 0, ctrX: 0, ctrY: 0 });
	const rawX = raw && chX >= 0 ? raw.channels[chX] : 0;
	const rawY = raw && chY >= 0 ? raw.channels[chY] : 0;

	useEffect(() => {
		if (!raw || calStage !== 'range') return;
		const d = calData.current;
		d.minX = Math.min(d.minX, rawX); d.maxX = Math.max(d.maxX, rawX);
		d.minY = Math.min(d.minY, rawY); d.maxY = Math.max(d.maxY, rawY);
	}, [raw, calStage, rawX, rawY]);

	const circle = (stick.diagScaleX ?? 0) === 0 && (stick.diagScaleY ?? 0) === 0;

	return (
		<div>
			<Card className="mb-3">
				<Card.Header>スティックスケーリング</Card.Header>
				<Card.Body>
					<h6 className="small text-muted">入力スケール</h6>
					<Row>
						<Col md={6}><NumRange label="上" min={0} max={200} value={stick.scaleUp}
							onChange={(v) => onChange({ ...stick, scaleUp: v })} /></Col>
						<Col md={6}><NumRange label="下" min={0} max={200} value={stick.scaleDown}
							onChange={(v) => onChange({ ...stick, scaleDown: v })} /></Col>
						<Col md={6}><NumRange label="左" min={0} max={200} value={stick.scaleLeft}
							onChange={(v) => onChange({ ...stick, scaleLeft: v })} /></Col>
						<Col md={6}><NumRange label="右" min={0} max={200} value={stick.scaleRight}
							onChange={(v) => onChange({ ...stick, scaleRight: v })} /></Col>
					</Row>
					<hr />
					<h6 className="small text-muted">スケーリング形状</h6>
					<div className="text-center mb-2">
						<ShapeSvg diagX={stick.diagScaleX ?? 0} diagY={stick.diagScaleY ?? 0} />
					</div>
					<Form.Group className="mb-2">
						<Form.Label>モード</Form.Label>
						<Form.Select value={circle ? 'circle' : 'normal'}
							onChange={(e) => {
								const v = e.target.value === 'circle' ? 0 : 25;
								onChange({ ...stick, diagScaleX: v, diagScaleY: v });
							}}>
							<option value="circle">真円に固定</option>
							<option value="normal">ノーマル（斜め拡張）</option>
						</Form.Select>
						<div className="text-muted small">真円: 斜め最大70.7% / ノーマル: 斜めを拡大（FireBird機の目安 X=Y=25）</div>
					</Form.Group>
					{!circle && (
						<Row>
							<Col md={6}><NumRange label="X" min={0} max={100} value={stick.diagScaleX ?? 0}
								onChange={(v) => onChange({ ...stick, diagScaleX: v })} /></Col>
							<Col md={6}><NumRange label="Y" min={0} max={100} value={stick.diagScaleY ?? 0}
								onChange={(v) => onChange({ ...stick, diagScaleY: v })} /></Col>
						</Row>
					)}
					<hr />
					<h6 className="small text-muted">軸設定</h6>
					<Row>
						<Col><Form.Check type="switch" label="X軸反転" checked={stick.invertX}
							onChange={(e) => onChange({ ...stick, invertX: e.target.checked })} /></Col>
						<Col><Form.Check type="switch" label="Y軸反転" checked={stick.invertY}
							onChange={(e) => onChange({ ...stick, invertY: e.target.checked })} /></Col>
					</Row>
				</Card.Body>
			</Card>

			<Card className="mb-3">
				<Card.Header>デッドゾーン</Card.Header>
				<Card.Body>
					<h6 className="small text-muted">インナーデッドゾーン</h6>
					<Row>
						<Col md={6}><NumRange label="X" min={0} max={50} value={stick.innerX}
							onChange={(v) => onChange({ ...stick, innerX: v })} /></Col>
						<Col md={6}><NumRange label="Y" min={0} max={50} value={stick.innerY}
							onChange={(v) => onChange({ ...stick, innerY: v })} /></Col>
					</Row>
					<h6 className="small text-muted mt-2">アウターデッドゾーン</h6>
					<Row>
						<Col md={6}><NumRange label="X" min={50} max={100} value={stick.outerX}
							onChange={(v) => onChange({ ...stick, outerX: v })} /></Col>
						<Col md={6}><NumRange label="Y" min={50} max={100} value={stick.outerY}
							onChange={(v) => onChange({ ...stick, outerY: v })} /></Col>
					</Row>
					<h6 className="small text-muted mt-2">アンチデッドゾーン</h6>
					<Row>
						<Col md={6}><NumRange label="X" min={0} max={50} value={stick.antiX}
							onChange={(v) => onChange({ ...stick, antiX: v })} /></Col>
						<Col md={6}><NumRange label="Y" min={0} max={50} value={stick.antiY}
							onChange={(v) => onChange({ ...stick, antiY: v })} /></Col>
					</Row>
				</Card.Body>
			</Card>

			<Card className="mb-3">
				<Card.Header>キャリブレーション</Card.Header>
				<Card.Body>
					<Row className="mb-2">
						<Col>X生値: <b>{rawX}</b> (min {stick.calMinX} / ctr {stick.calCenterX} / max {stick.calMaxX})</Col>
						<Col>Y生値: <b>{rawY}</b> (min {stick.calMinY} / ctr {stick.calCenterY} / max {stick.calMaxY})</Col>
					</Row>
					{calStage === 'idle' && (
						<Button size="sm" onClick={() => {
							calData.current = { minX: 65535, maxX: 0, minY: 65535, maxY: 0, ctrX: rawX, ctrY: rawY };
							setCalStage('center');
						}}>キャリブレーション開始</Button>
					)}
					{calStage === 'center' && (
						<Alert variant="info" className="p-2 mb-0">
							スティックから手を離して →
							<Button size="sm" className="ms-2" onClick={() => {
								calData.current.ctrX = rawX; calData.current.ctrY = rawY;
								setCalStage('range');
							}}>中心を記録</Button>
						</Alert>
					)}
					{calStage === 'range' && (
						<Alert variant="info" className="p-2 mb-0">
							スティックを数回ゆっくり全周回して →
							<Button size="sm" className="ms-2" onClick={() => {
								const d = calData.current;
								onChange({
									...stick,
									calMinX: d.minX, calCenterX: d.ctrX, calMaxX: d.maxX,
									calMinY: d.minY, calCenterY: d.ctrY, calMaxY: d.maxY,
								});
								setCalStage('idle');
							}}>完了・記録</Button>
							<div className="small mt-1">
								X: {calData.current.minX}〜{calData.current.maxX} /
								Y: {calData.current.minY}〜{calData.current.maxY}
							</div>
						</Alert>
					)}
				</Card.Body>
			</Card>
		</div>
	);
}

// 詳細設定 tab: sensitivity curve + output correction (resolution / delay)
function DetailTab({
	stick, onChange,
}: { stick: StickOpts; onChange: (s: StickOpts) => void }) {
	return (
		<div>
			<Card className="mb-3">
				<Card.Header>感度カーブ</Card.Header>
				<Card.Body>
					<CurveEditor stick={stick} onChange={onChange} />
				</Card.Body>
			</Card>
			<Card className="mb-3">
				<Card.Header>出力補正</Card.Header>
				<Card.Body>
					<NumRange label="スティック分解能 (0 = 無制限)" min={0} max={256}
						value={stick.resolution}
						suffix="全域を何段階に量子化するか。低いほど段階的な出力になります。"
						onChange={(v) => onChange({ ...stick, resolution: v })} />
					<NumRange label="追加入力遅延 (ms)" min={0} max={30} value={stick.delayMs}
						onChange={(v) => onChange({ ...stick, delayMs: v })} />
				</Card.Body>
			</Card>
		</div>
	);
}

function StickPanel({
	stick, chX, chY, raw, onChange,
}: {
	stick: StickOpts;
	chX: number; chY: number;
	raw: RawData | null;
	onChange: (s: StickOpts) => void;
}) {
	return (
		<Tabs defaultActiveKey="basic" className="mb-3">
			<Tab eventKey="basic" title="基本設定">
				<BasicTab stick={stick} chX={chX} chY={chY} raw={raw} onChange={onChange} />
			</Tab>
			<Tab eventKey="detail" title="詳細設定">
				<DetailTab stick={stick} onChange={onChange} />
			</Tab>
			<Tab eventKey="filter" title="フィルター">
				<FilterTab stick={stick} chX={chX} chY={chY} onChange={onChange} />
			</Tab>
			<Tab eventKey="angle" title="角度補正">
				<AngleTab stick={stick} onChange={onChange} />
			</Tab>
		</Tabs>
	);
}

export default function HMLAnalogPage() {
	const [opts, setOpts] = useState<Options | null>(null);
	const [raw, setRaw] = useState<RawData | null>(null);
	const [saveMsg, setSaveMsg] = useState('');

	useEffect(() => {
		Http.get(`${baseUrl}/api/getHMLAnalogOptions`).then(({ data }) => {
			const fill = (s: Partial<StickOpts> | undefined): StickOpts => ({ ...defaultStick(), ...(s || {}) });
			setOpts({
				...data,
				leftStick: fill(data.leftStick),
				rightStick: fill(data.rightStick),
			});
		}).catch(console.error);
	}, []);

	useEffect(() => {
		// Poll fast so the stick view feels realtime; the gauge smooths between
		// samples with requestAnimationFrame. Guard against overlap so a slow
		// response can't queue up a backlog of requests.
		let alive = true;
		let inflight = false;
		const poll = () => {
			if (!alive || inflight) return;
			inflight = true;
			Http.get(`${baseUrl}/api/getHMLAnalogRaw`)
				.then(({ data }) => { if (alive) setRaw(data); })
				.catch(() => {})
				.finally(() => { inflight = false; });
		};
		const id = setInterval(poll, 45);
		return () => { alive = false; clearInterval(id); };
	}, []);

	const save = useCallback(async () => {
		if (!opts) return;
		try {
			// This page only owns the two sticks. Re-fetch and overwrite just the
			// stick fields so channel/lightbar/dongle edits from other tabs are
			// preserved (GET-merge-POST).
			const { data } = await Http.get(`${baseUrl}/api/getHMLAnalogOptions`);
			const {
				lightbarEnabled, lightbarDataPin, lightbarBrightness, lightbarColor,
				usbHostEnabled, usbHostDp, ...body
			} = { ...data, leftStick: opts.leftStick, rightStick: opts.rightStick };
			await Http.post(`${baseUrl}/api/setHMLAnalogOptions`, body);
			setSaveMsg('保存しました。再起動後に有効になります。');
		} catch {
			setSaveMsg('保存に失敗しました');
		}
		setTimeout(() => setSaveMsg(''), 4000);
	}, [opts]);

	if (!opts) return <div>Loading...</div>;

	return (
		<div>
			<Card className="mb-3">
				<Card.Header>ライブテスター</Card.Header>
				<Card.Body>
					{raw?.valid ? (
						<GamepadTester raw={raw} opts={opts} />
					) : (
						<Alert variant="warning" className="mb-0">
							ライブ値が取得できません。web-configモード中のみ表示されます。
						</Alert>
					)}
				</Card.Body>
			</Card>

			<Tabs defaultActiveKey="left" className="mb-3">
				<Tab eventKey="left" title="左スティック">
					<StickPanel stick={opts.leftStick}
						chX={opts.chLX} chY={opts.chLY} raw={raw}
						onChange={(s) => setOpts({ ...opts, leftStick: s })} />
				</Tab>
				<Tab eventKey="right" title="右スティック">
					<StickPanel stick={opts.rightStick}
						chX={opts.chRX} chY={opts.chRY} raw={raw}
						onChange={(s) => setOpts({ ...opts, rightStick: s })} />
				</Tab>
			</Tabs>

			<Button onClick={save}>保存</Button>
			{saveMsg && <Alert variant="success" className="mt-2 p-2">{saveMsg}</Alert>}
		</div>
	);
}
