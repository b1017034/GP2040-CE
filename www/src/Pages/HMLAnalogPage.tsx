import { useEffect, useRef, useState, useCallback } from 'react';
import {
	Alert,
	Button,
	Card,
	Col,
	Form,
	ProgressBar,
	Row,
	Tab,
	Tabs,
} from 'react-bootstrap';
import Http from '../Services/Http';
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
};

type Options = {
	enabled: number | boolean;
	chLX: number; chLY: number; chRX: number; chRY: number;
	chPsButton: number; psThreshold: number; psActiveHigh: boolean;
	ltAdcPin: number; rtAdcPin: number;
	leftStick: StickOpts; rightStick: StickOpts;
};

type RawData = {
	channels: number[];
	adc: number[];
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
	rcFilterStrength: 0, rcSnapback: 10, rcAdvanced: false,
	rcCurvePointCount: 3, rcCurveX: [32, 96, 128], rcCurveY: [0, 0, 0],
	rcVelocityMult100: 100,
	invertX: false, invertY: false,
});

const CHANNEL_OPTIONS = [-1, 0, 1, 2, 3, 4, 5, 6, 7];

function CurveEditor({
	stick, onChange,
}: { stick: StickOpts; onChange: (s: StickOpts) => void }) {
	const isCustom = stick.curvePreset >= 5;
	const pts = isCustom
		? stick.curveX.map((x, i) => [x, stick.curveY[i] ?? 100])
		: CURVE_PRESETS[stick.curvePreset]?.pts ?? CURVE_PRESETS[0].pts;
	const count = isCustom
		? Math.min(stick.curvePointCount || pts.length, pts.length)
		: pts.length;

	// SVG path: 0,0 -> points, viewport 200x200 (y flipped)
	const path = ['M 0 200']
		.concat(pts.slice(0, count).map(([x, y]) => `L ${x * 2} ${200 - y * 2}`))
		.join(' ');

	return (
		<div>
			<Form.Group className="mb-2">
				<Form.Label>感度カーブ</Form.Label>
				<Form.Select
					value={stick.curvePreset}
					onChange={(e) => onChange({ ...stick, curvePreset: +e.target.value })}
				>
					{Object.entries(CURVE_PRESETS).map(([v, p]) => (
						<option key={v} value={v}>{p.label}</option>
					))}
				</Form.Select>
			</Form.Group>
			<svg viewBox="-8 -8 216 216" style={{ width: 180, background: 'var(--bs-secondary-bg)', borderRadius: 4 }}>
				<rect x="0" y="0" width="200" height="200" fill="none" stroke="#888" strokeWidth="1" />
				<line x1="0" y1="200" x2="200" y2="0" stroke="#555" strokeDasharray="4" />
				<path d={path} fill="none" stroke="#4da3ff" strokeWidth="3" />
				{pts.slice(0, count).map(([x, y], i) => (
					<circle key={i} cx={x * 2} cy={200 - y * 2} r="5" fill="#ffc107" />
				))}
			</svg>
			{isCustom && (
				<>
					<Form.Group className="mb-2 mt-2">
						<Form.Label>ポイント数: {stick.curvePointCount}</Form.Label>
						<Form.Range min={1} max={5} value={stick.curvePointCount}
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

function RcFilterEditor({
	stick, onChange,
}: { stick: StickOpts; onChange: (s: StickOpts) => void }) {
	return (
		<div>
			<h6 className="mt-3">RCフィルター</h6>
			<p className="small text-muted">
				＋方向: ジッター減・応答は鈍く ／ −方向: 応答鋭く・ジッター増（Firebird互換 -500〜500）
			</p>
			<Form.Group className="mb-2">
				<Form.Label>フィルター強度: {stick.rcFilterStrength}</Form.Label>
				<Form.Range min={-500} max={500} value={stick.rcFilterStrength}
					onChange={(e) => onChange({ ...stick, rcFilterStrength: +e.target.value })} />
			</Form.Group>
			<Form.Check type="switch" label="上級モード（速度依存カーブ）"
				checked={stick.rcAdvanced}
				onChange={(e) => onChange({ ...stick, rcAdvanced: e.target.checked })} />
			{stick.rcAdvanced && (
				<>
					<Form.Group className="mb-2 mt-2">
						<Form.Label>RCフィルター補正 (snapback): {stick.rcSnapback}</Form.Label>
						<Form.Range min={0} max={100} value={stick.rcSnapback}
							onChange={(e) => onChange({ ...stick, rcSnapback: +e.target.value })} />
						<Form.Text>10 = フィルター値をそのまま使用。大きいほど過補正（スナップバック抑制）。</Form.Text>
					</Form.Group>
					<Form.Group className="mb-2">
						<Form.Label>速度の倍率: {(stick.rcVelocityMult100 / 100).toFixed(2)}</Form.Label>
						<Form.Range min={0} max={1000} value={stick.rcVelocityMult100}
							onChange={(e) => onChange({ ...stick, rcVelocityMult100: +e.target.value })} />
					</Form.Group>
					<Form.Label className="small">速度(0〜128)→強度カーブ</Form.Label>
					{Array.from({ length: stick.rcCurvePointCount }).map((_, i) => (
						<Row key={i} className="mb-1">
							<Col xs={2}>P{i + 1}</Col>
							<Col>
								<Form.Label className="small">速度 {stick.rcCurveX[i] ?? 128}</Form.Label>
								<Form.Range min={0} max={128} value={stick.rcCurveX[i] ?? 128}
									onChange={(e) => {
										const v = [...stick.rcCurveX]; v[i] = +e.target.value;
										onChange({ ...stick, rcCurveX: v });
									}} />
							</Col>
							<Col>
								<Form.Label className="small">強度 {stick.rcCurveY[i] ?? 0}</Form.Label>
								<Form.Range min={-500} max={500} value={stick.rcCurveY[i] ?? 0}
									onChange={(e) => {
										const v = [...stick.rcCurveY]; v[i] = +e.target.value;
										onChange({ ...stick, rcCurveY: v });
									}} />
							</Col>
						</Row>
					))}
				</>
			)}
		</div>
	);
}

function StickPanel({
	label, stick, chX, chY, raw, onChange,
}: {
	label: string;
	stick: StickOpts;
	chX: number; chY: number;
	raw: RawData | null;
	onChange: (s: StickOpts) => void;
}) {
	const [calStage, setCalStage] = useState<'idle' | 'center' | 'range'>('idle');
	const calData = useRef({ minX: 65535, maxX: 0, minY: 65535, maxY: 0, ctrX: 0, ctrY: 0 });

	const rawX = raw && chX >= 0 ? raw.channels[chX] : 0;
	const rawY = raw && chY >= 0 ? raw.channels[chY] : 0;

	useEffect(() => {
		if (!raw) return;
		if (calStage === 'range') {
			const d = calData.current;
			d.minX = Math.min(d.minX, rawX); d.maxX = Math.max(d.maxX, rawX);
			d.minY = Math.min(d.minY, rawY); d.maxY = Math.max(d.maxY, rawY);
		}
	}, [raw, calStage, rawX, rawY]);

	return (
		<div>
			<h6>キャリブレーション</h6>
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
				<Alert variant="info" className="p-2">
					スティックから手を離して →
					<Button size="sm" className="ms-2" onClick={() => {
						calData.current.ctrX = rawX;
						calData.current.ctrY = rawY;
						setCalStage('range');
					}}>中心を記録</Button>
				</Alert>
			)}
			{calStage === 'range' && (
				<Alert variant="info" className="p-2">
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
			<Row className="mt-2">
				<Col>
					<Form.Check type="switch" label="X反転" checked={stick.invertX}
						onChange={(e) => onChange({ ...stick, invertX: e.target.checked })} />
				</Col>
				<Col>
					<Form.Check type="switch" label="Y反転" checked={stick.invertY}
						onChange={(e) => onChange({ ...stick, invertY: e.target.checked })} />
				</Col>
			</Row>
			<Form.Group className="mb-2 mt-2">
				<Form.Label>内側デッドゾーン: {stick.innerDeadzone}%</Form.Label>
				<Form.Range min={0} max={40} value={stick.innerDeadzone}
					onChange={(e) => onChange({ ...stick, innerDeadzone: +e.target.value })} />
			</Form.Group>
			<Form.Group className="mb-2">
				<Form.Label>外側デッドゾーン: {stick.outerDeadzone}%</Form.Label>
				<Form.Range min={0} max={40} value={stick.outerDeadzone}
					onChange={(e) => onChange({ ...stick, outerDeadzone: +e.target.value })} />
			</Form.Group>
			<CurveEditor stick={stick} onChange={onChange} />
			<RcFilterEditor stick={stick} onChange={onChange} />
		</div>
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
		const id = setInterval(() => {
			Http.get(`${baseUrl}/api/getHMLAnalogRaw`)
				.then(({ data }) => setRaw(data))
				.catch(() => {});
		}, 150);
		return () => clearInterval(id);
	}, []);

	const save = useCallback(async () => {
		if (!opts) return;
		try {
			await Http.post(`${baseUrl}/api/setHMLAnalogOptions`, opts);
			setSaveMsg('保存しました。再起動後に有効になります。');
		} catch {
			setSaveMsg('保存に失敗しました');
		}
		setTimeout(() => setSaveMsg(''), 4000);
	}, [opts]);

	if (!opts) return <div>Loading...</div>;

	const chSelect = (key: keyof Options, label: string) => (
		<Form.Group as={Col} md={2} className="mb-2">
			<Form.Label>{label}</Form.Label>
			<Form.Select value={opts[key] as number}
				onChange={(e) => setOpts({ ...opts, [key]: +e.target.value })}>
				{CHANNEL_OPTIONS.map((c) => (
					<option key={c} value={c}>{c < 0 ? '無効' : `CH${c}`}</option>
				))}
			</Form.Select>
		</Form.Group>
	);

	return (
		<div>
			<h1>HML スティック設定</h1>
			<Card className="mb-3">
				<Card.Header>ADS8332 ライブ生値（チャンネル特定に使用）</Card.Header>
				<Card.Body>
					{raw?.valid ? (
						<Row>
							{raw.channels.map((v, i) => (
								<Col key={i} xs={3} className="mb-2">
									<div className="small">CH{i}: {v}</div>
									<ProgressBar now={(v / 65535) * 100} />
								</Col>
							))}
							<Col xs={3} className="mb-2">
								<div className="small">ADC26 (LT): {raw.adc[0]}</div>
								<ProgressBar variant="info" now={(raw.adc[0] / 4095) * 100} />
							</Col>
							<Col xs={3} className="mb-2">
								<div className="small">ADC27 (RT): {raw.adc[1]}</div>
								<ProgressBar variant="info" now={(raw.adc[1] / 4095) * 100} />
							</Col>
						</Row>
					) : (
						<Alert variant="warning" className="mb-0">
							アドオンが動作していません（webconfigモード中はライブ値は取得できません。通常起動でスティックを動かして確認する場合はゲームパッドテスターを使用）。
							チャンネル値がすべて0の場合はSPI通信を確認してください。
						</Alert>
					)}
				</Card.Body>
			</Card>

			<Card className="mb-3">
				<Card.Header>チャンネル割り当て</Card.Header>
				<Card.Body>
					<Form.Check type="switch" label="アドオン有効" className="mb-2"
						checked={!!opts.enabled}
						onChange={(e) => setOpts({ ...opts, enabled: e.target.checked ? 1 : 0 })} />
					<Row>
						{chSelect('chLX', '左スティックX')}
						{chSelect('chLY', '左スティックY')}
						{chSelect('chRX', '右スティックX')}
						{chSelect('chRY', '右スティックY')}
						{chSelect('chPsButton', 'PSボタン')}
					</Row>
					<Row>
						<Form.Group as={Col} md={3}>
							<Form.Label>PS判定しきい値: {opts.psThreshold}</Form.Label>
							<Form.Range min={0} max={65535} value={opts.psThreshold}
								onChange={(e) => setOpts({ ...opts, psThreshold: +e.target.value })} />
						</Form.Group>
						<Form.Group as={Col} md={3}>
							<Form.Check type="switch" label="押下で値が上がる (active high)"
								checked={opts.psActiveHigh}
								onChange={(e) => setOpts({ ...opts, psActiveHigh: e.target.checked })} />
						</Form.Group>
					</Row>
				</Card.Body>
			</Card>

			<Tabs defaultActiveKey="left" className="mb-3">
				<Tab eventKey="left" title="左スティック">
					<StickPanel label="左" stick={opts.leftStick}
						chX={opts.chLX} chY={opts.chLY} raw={raw}
						onChange={(s) => setOpts({ ...opts, leftStick: s })} />
				</Tab>
				<Tab eventKey="right" title="右スティック">
					<StickPanel label="右" stick={opts.rightStick}
						chX={opts.chRX} chY={opts.chRY} raw={raw}
						onChange={(s) => setOpts({ ...opts, rightStick: s })} />
				</Tab>
			</Tabs>

			<Button onClick={save}>保存</Button>
			{saveMsg && <Alert variant="success" className="mt-2 p-2">{saveMsg}</Alert>}
		</div>
	);
}
