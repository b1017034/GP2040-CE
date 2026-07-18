// Standalone control for the HML PS5 (P5 General) auth-dongle USB host toggle.
// Fetches/saves the HML analog options itself so it can be its own Settings tab.
import { useEffect, useState, useCallback } from 'react';
import { Alert, Button, Form } from 'react-bootstrap';
import Http from '../Services/Http';
import { baseUrl } from '../Services/WebApi';

type Dongle = { usbHostEnabled: boolean; usbHostDp: number };

export default function HMLDongleControl() {
	const [d, setD] = useState<Dongle | null>(null);
	const [msg, setMsg] = useState('');

	useEffect(() => {
		Http.get(`${baseUrl}/api/getHMLAnalogOptions`)
			.then(({ data }) => setD({
				usbHostEnabled: !!data.usbHostEnabled,
				usbHostDp: data.usbHostDp ?? 6,
			}))
			.catch(console.error);
	}, []);

	const save = useCallback(async () => {
		if (!d) return;
		try {
			const { data } = await Http.get(`${baseUrl}/api/getHMLAnalogOptions`);
			await Http.post(`${baseUrl}/api/setHMLAnalogOptions`, { ...data, ...d });
			setMsg('保存しました。再起動後に有効になります。');
		} catch {
			setMsg('保存に失敗しました');
		}
		setTimeout(() => setMsg(''), 4000);
	}, [d]);

	if (!d) return <div>Loading...</div>;
	const dp = d.usbHostDp >= 0 ? d.usbHostDp : 6;

	return (
		<div>
			<Form.Check type="switch"
				label={`USBホストを有効化（ドングル用ポート D+ = GPIO${dp} / D− = GPIO${dp + 1}）`}
				checked={!!d.usbHostEnabled}
				onChange={(e) => setD({ ...d, usbHostEnabled: e.target.checked })} />
			<Form.Text className="text-muted d-block mb-3">
				P5 Generalモード（R2ホールド起動）はドングル(VID 0x2B81)が必須。
				このスイッチは「周辺機器マッピング」のUSB0設定と連動します。保存後、再起動で有効。
			</Form.Text>
			<Button onClick={save}>保存</Button>
			{msg && <Alert variant="success" className="mt-2 p-2">{msg}</Alert>}
		</div>
	);
}
