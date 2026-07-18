import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import path from "path";

// import dev host for wsl2
const host = process.env.VITE_DEV_HOST || 'localhost';

// When VITE_PROXY_TARGET is set (e.g. the board at http://192.168.7.1), Vite
// proxies /api and /images to it so the browser talks same-origin to the dev
// server — no CORS, works through WSL2. Used by `npm run dev-board`.
const proxyTarget = process.env.VITE_PROXY_TARGET;
const proxy = proxyTarget
	? {
			'/api': { target: proxyTarget, changeOrigin: true },
			'/images': { target: proxyTarget, changeOrigin: true },
		}
	: undefined;

// https://vitejs.dev/config/
export default defineConfig({
	build: {
		outDir: path.join(__dirname, "build"),
		sourcemap: false,
	},
	server: {
		host: host,
		open: true,
		port: 3000,
		proxy,
	},
	plugins: [react()],
	resolve: {
		alias: {
			"~bootstrap": path.resolve(__dirname, "node_modules/bootstrap"),
			lodash: 'lodash-es',
			"@proto": path.resolve(__dirname, "src_gen"),
		},
	},
});
