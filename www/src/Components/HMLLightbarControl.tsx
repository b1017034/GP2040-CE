// Shared control for the HML lightbar LED. Reads/writes the HML analog options
// endpoint so it can live on both the HML page and the general Settings page.
import { useEffect, useState, useCallback } from 'react';
import { Alert, Button, Col, Form, Row } from 'react-bootstrap';
import Http from '../Services/Http';
import { baseUrl } from '../Services/WebApi';

type Lightbar = {
	lightbarEnabled: boolean;
	lightbarDataPin: number;
	lightbarBrightness: number;
	lightbarColor: number; // 0xRRGGBB
};

const toHex = (v: number) => `#${(v & 0xffffff).toString(16).padStart(6, '0')}`;
const fromHex = (s: string) => parseInt(s.replace('#', ''), 16) & 0xffffff;

// `embedded` mode assumes the parent already fetches/saves the HML options and
// passes value/onChange. Standalone mode (Settings page) fetches + saves itself.
export default function HMLLightbarControl({
	value, onChange,
}: {
	value?: Lightbar;
	onChange?: (patch: Partial<Lightbar>) => void;
} = {}) {
	const embedded = value !== undefined && onChange !== undefined;
	const [local, setLocal] = useState<Lightbar | null>(null);
	const [msg, setMsg] = useState('');

	useEffect(() => {
		if (embedded) return;
		Http.get(`${baseUrl}/api/getHMLAnalogOptions`)
			.then(({ data }) => setLocal({
				lightbarEnabled: !!data.lightbarEnabled,
				lightbarDataPin: data.lightbarDataPin ?? 20,
				lightbarBrightness: data.lightbarBrightness ?? 128,
				lightbarColor: data.lightbarColor ?? 0x000040,
			}))
			.catch(console.error);
	}, [embedded]);

	const lb = (embedded ? value : local) as Lightbar | null;
	const patch = useCallback((p: Partial<Lightbar>) => {
		if (embedded) onChange!(p);
		else setLocal((s) => (s ? { ...s, ...p } : s));
	}, [embedded, onChange]);

	const saveStandalone = useCallback(async () => {
		if (!local) return;
		try {
			// merge into the full HML options so we don't wipe other fields
			const { data } = await Http.get(`${baseUrl}/api/getHMLAnalogOptions`);
			await Http.post(`${baseUrl}/api/setHMLAnalogOptions`, { ...data, ...local });
			setMsg('保存しました。再起動後、この色で常時点灯します。');
		} catch {
			setMsg('保存に失敗しました');
		}
		setTimeout(() => setMsg(''), 4000);
	}, [local]);

	if (!lb) return <div>Loading...</div>;

	return (
		<div>
			<Form.Check type="switch"
				label="ライトバーLEDを有効化（指定した色で常時点灯）"
				checked={!!lb.lightbarEnabled}
				onChange={(e) => patch({
					lightbarEnabled: e.target.checked,
					...(e.target.checked && lb.lightbarDataPin < 0 ? { lightbarDataPin: 20 } : {}),
				})} />
			{lb.lightbarEnabled && (
				<Row className="mt-2 align-items-end">
					<Form.Group as={Col} xs={6} md={3}>
						<Form.Label>色</Form.Label>
						<Form.Control type="color" value={toHex(lb.lightbarColor)}
							onChange={(e) => patch({ lightbarColor: fromHex(e.target.value) })}
							style={{ height: 38 }} />
						<Form.Text className="text-muted">この色で常時点灯します。</Form.Text>
					</Form.Group>
					<Form.Group as={Col} xs={6} md={4}>
						<Form.Label>明るさ上限: {lb.lightbarBrightness}</Form.Label>
						<Form.Range min={0} max={255} value={lb.lightbarBrightness}
							onChange={(e) => patch({ lightbarBrightness: +e.target.value })} />
					</Form.Group>
					<Form.Group as={Col} xs={6} md={3}>
						<Form.Label>データピン (GPIO)</Form.Label>
						<Form.Control type="number" min={-1} max={29}
							value={lb.lightbarDataPin >= 0 ? lb.lightbarDataPin : 20}
							onChange={(e) => patch({ lightbarDataPin: +e.target.value })} />
						<Form.Text className="text-muted">HML標準はGPIO20。</Form.Text>
					</Form.Group>
				</Row>
			)}
			{!embedded && (
				<div className="mt-3">
					<Button onClick={saveStandalone}>保存</Button>
					{msg && <Alert variant="success" className="mt-2 p-2">{msg}</Alert>}
				</div>
			)}
		</div>
	);
}
