// HML device / hardware settings: ADS8332 channel assignment + live raw
// monitor + PS button threshold. Fetches/saves the HML analog options itself
// (GET-merge-POST so it never clobbers the stick-settings tab).
import { useEffect, useState, useCallback } from 'react';
import { Alert, Button, Card, Col, Form, ProgressBar, Row } from 'react-bootstrap';
import Http from '../Services/Http';
import { baseUrl } from '../Services/WebApi';

type Dev = {
	enabled: number | boolean;
	chLX: number; chLY: number; chRX: number; chRY: number;
	chPsButton: number; psThreshold: number; psActiveHigh: boolean;
	ltAdcPin: number; rtAdcPin: number;
};

type RawData = {
	channels: number[]; adc: number[]; valid: boolean;
};

const CHANNEL_OPTIONS = [-1, 0, 1, 2, 3, 4, 5, 6, 7];

export default function HMLDeviceControl() {
	const [dev, setDev] = useState<Dev | null>(null);
	const [raw, setRaw] = useState<RawData | null>(null);
	const [msg, setMsg] = useState('');

	useEffect(() => {
		Http.get(`${baseUrl}/api/getHMLAnalogOptions`)
			.then(({ data }) => setDev({
				enabled: data.enabled, chLX: data.chLX, chLY: data.chLY,
				chRX: data.chRX, chRY: data.chRY, chPsButton: data.chPsButton,
				psThreshold: data.psThreshold, psActiveHigh: !!data.psActiveHigh,
				ltAdcPin: data.ltAdcPin, rtAdcPin: data.rtAdcPin,
			}))
			.catch(console.error);
	}, []);

	useEffect(() => {
		let alive = true, inflight = false;
		const poll = () => {
			if (!alive || inflight) return;
			inflight = true;
			Http.get(`${baseUrl}/api/getHMLAnalogRaw`)
				.then(({ data }) => { if (alive) setRaw(data); })
				.catch(() => {})
				.finally(() => { inflight = false; });
		};
		const id = setInterval(poll, 80);
		return () => { alive = false; clearInterval(id); };
	}, []);

	const save = useCallback(async () => {
		if (!dev) return;
		try {
			const { data } = await Http.get(`${baseUrl}/api/getHMLAnalogOptions`);
			// keep other tabs' domains (sticks, lightbar, dongle) intact
			const { usbHostEnabled, usbHostDp, ...body } = { ...data, ...dev,
				enabled: dev.enabled ? 1 : 0 };
			await Http.post(`${baseUrl}/api/setHMLAnalogOptions`, body);
			setMsg('保存しました。再起動後に有効になります。');
		} catch {
			setMsg('保存に失敗しました');
		}
		setTimeout(() => setMsg(''), 4000);
	}, [dev]);

	if (!dev) return <div>Loading...</div>;

	const chSelect = (key: keyof Dev, label: string) => (
		<Form.Group as={Col} md={2} className="mb-2">
			<Form.Label>{label}</Form.Label>
			<Form.Select value={dev[key] as number}
				onChange={(e) => setDev({ ...dev, [key]: +e.target.value })}>
				{CHANNEL_OPTIONS.map((c) => (
					<option key={c} value={c}>{c < 0 ? '無効' : `CH${c}`}</option>
				))}
			</Form.Select>
		</Form.Group>
	);

	return (
		<div>
			<Card className="mb-3">
				<Card.Header>チャンネル割り当て</Card.Header>
				<Card.Body>
					<Form.Check type="switch" label="アドオン有効" className="mb-2"
						checked={!!dev.enabled}
						onChange={(e) => setDev({ ...dev, enabled: e.target.checked ? 1 : 0 })} />
					<Row>
						{chSelect('chLX', '左スティックX')}
						{chSelect('chLY', '左スティックY')}
						{chSelect('chRX', '右スティックX')}
						{chSelect('chRY', '右スティックY')}
						{chSelect('chPsButton', 'PSボタン')}
					</Row>
					<Row>
						<Form.Group as={Col} md={4}>
							<Form.Label>PS判定しきい値: {dev.psThreshold}</Form.Label>
							<Form.Range min={0} max={65535} value={dev.psThreshold}
								onChange={(e) => setDev({ ...dev, psThreshold: +e.target.value })} />
						</Form.Group>
						<Form.Group as={Col} md={4}>
							<Form.Check type="switch" label="押下で値が上がる (active high)"
								checked={dev.psActiveHigh}
								onChange={(e) => setDev({ ...dev, psActiveHigh: e.target.checked })} />
						</Form.Group>
					</Row>
				</Card.Body>
			</Card>

			<Card className="mb-3">
				<Card.Header>ADS8332 生値（チャンネル特定用）</Card.Header>
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
								<div className="small">ADC{dev.ltAdcPin} (LT): {raw.adc[0]}</div>
								<ProgressBar variant="info" now={(raw.adc[0] / 4095) * 100} />
							</Col>
							<Col xs={3} className="mb-2">
								<div className="small">ADC{dev.rtAdcPin} (RT): {raw.adc[1]}</div>
								<ProgressBar variant="info" now={(raw.adc[1] / 4095) * 100} />
							</Col>
						</Row>
					) : (
						<Alert variant="warning" className="mb-0">
							チャンネル値がすべて0の場合はSPI通信を確認してください。
						</Alert>
					)}
				</Card.Body>
			</Card>

			<Button onClick={save}>保存</Button>
			{msg && <Alert variant="success" className="mt-2 p-2">{msg}</Alert>}
		</div>
	);
}
