#!/usr/bin/env python3
"""Meteoris browser gateway: DSP TCP client + HTTP/WebSocket UI."""

import argparse
import asyncio
import json
import pathlib
import re
import sys

try:
    from aiohttp import web
except ImportError as exc:
    raise SystemExit("meteoris_web requires aiohttp: python -m pip install aiohttp") from exc

HEADER_BYTES = 68

def meteoris_version() -> str:
    here = pathlib.Path(__file__).resolve()
    candidates = (
        here.parent / "VERSION",
        here.parent.parent / "VERSION",
        here.parent.parent / "share" / "meteoris" / "VERSION",
        pathlib.Path(sys.prefix) / "share" / "meteoris" / "VERSION",
    )
    for version_file in candidates:
        try:
            value = version_file.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", value):
            return value
    raise RuntimeError("Cannot determine Meteoris version: VERSION file not found")


class ControlClient:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.reader = None
        self.writer = None
        self.lock = asyncio.Lock()

    async def close(self):
        if self.writer is not None:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except Exception:
                pass
        self.reader = self.writer = None

    async def _connect(self):
        if self.writer is not None and not self.writer.is_closing():
            return
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)
        hello = (await self.reader.readline()).decode("utf-8", "replace").strip()
        if hello != "HELLO METEORIS 1":
            await self.close()
            raise RuntimeError(f"unexpected DSP control greeting: {hello!r}")

    async def request(self, command: str):
        async with self.lock:
            for attempt in range(2):
                try:
                    await self._connect()
                    self.writer.write((command + "\n").encode("utf-8"))
                    await self.writer.drain()
                    line = (await self.reader.readline()).decode("utf-8", "replace").strip()
                    if not line:
                        raise ConnectionError("DSP control connection closed")
                    if line.startswith("CONFIG "):
                        size = int(line.split()[1])
                        return "CONFIG", (await self.reader.readexactly(size)).decode("utf-8", "replace")
                    if line.startswith("STATUS "):
                        return "STATUS", json.loads(line[7:])
                    if line.startswith("OK "):
                        return "OK", line[3:]
                    if line.startswith("ERR "):
                        return "ERR", line[4:]
                    return "RAW", line
                except (OSError, asyncio.IncompleteReadError, ConnectionError):
                    await self.close()
                    if attempt:
                        raise
            raise ConnectionError("DSP control unavailable")


class Gateway:
    def __init__(self, dsp_host: str, psd_port: int, control_port: int):
        self.dsp_host = dsp_host
        self.psd_port = psd_port
        self.control = ControlClient(dsp_host, control_port)
        self.websockets = set()
        self.psd_connected = False
        self.psd_task = None
        # The upstream PSD socket is demand-driven.  Keeping it connected while
        # no browser is watching would make meteoris serialize/transmit every
        # PSD frame for no useful work (significant on a Raspberry Pi).
        self._psd_wanted = asyncio.Event()

    async def start(self, app):
        self.psd_task = asyncio.create_task(self._psd_loop())

    async def stop(self, app):
        if self.psd_task:
            self.psd_task.cancel()
            try:
                await self.psd_task
            except asyncio.CancelledError:
                pass
        await self.control.close()

    async def _psd_loop(self):
        while True:
            # Do not connect to the DSP PSD port until at least one browser has
            # subscribed to /ws/psd.  Closing the last browser tears the socket
            # down on the next PSD (normally within ~8 ms), causing the DSP-side
            # network publisher to become a no-op again.
            await self._psd_wanted.wait()
            writer = None
            try:
                reader, writer = await asyncio.open_connection(self.dsp_host, self.psd_port)
                self.psd_connected = True
                while self.websockets:
                    header = await reader.readexactly(HEADER_BYTES)
                    if header[:4] != b"MPSD":
                        raise RuntimeError("invalid PSD stream magic")
                    payload_bytes = int.from_bytes(header[8:12], "little")
                    payload = await reader.readexactly(payload_bytes)
                    frame = header + payload
                    dead = []
                    for ws in tuple(self.websockets):
                        try:
                            await ws.send_bytes(frame)
                        except Exception:
                            dead.append(ws)
                    for ws in dead:
                        self.websockets.discard(ws)
                    if not self.websockets:
                        self._psd_wanted.clear()
            except asyncio.CancelledError:
                raise
            except Exception:
                self.psd_connected = False
                # Retry only while a browser still wants the stream.  This also
                # avoids a permanent reconnect loop after the last viewer exits.
                if self.websockets:
                    await asyncio.sleep(1.0)
                else:
                    self._psd_wanted.clear()
            finally:
                self.psd_connected = False
                if writer is not None:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass


def find_web_root(explicit: str | None) -> pathlib.Path:
    if explicit:
        root = pathlib.Path(explicit).resolve()
        if (root / "index.html").is_file():
            return root
        raise SystemExit(f"web root does not contain index.html: {root}")
    here = pathlib.Path(__file__).resolve().parent
    candidates = [
        here,
        here.parent / "share" / "meteoris" / "web",
        pathlib.Path(sys.prefix) / "share" / "meteoris" / "web",
        pathlib.Path("/usr/local/share/meteoris/web"),
        pathlib.Path("/usr/share/meteoris/web"),
    ]
    for root in candidates:
        if (root / "index.html").is_file():
            return root
    raise SystemExit("cannot locate Meteoris web assets; use --web-root")


def make_app(gateway: Gateway, web_root: pathlib.Path, listen_ip: str) -> web.Application:
    app = web.Application()

    def static_response(path: pathlib.Path) -> web.FileResponse:
        # The web UI is intentionally not cached. Meteoris is frequently
        # updated in-place on observatory/Raspberry Pi systems; stale browser
        # copies of index.html/app.js/style.css otherwise make a freshly
        # installed UI appear unchanged.
        response = web.FileResponse(path)
        response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
        response.headers["Pragma"] = "no-cache"
        response.headers["Expires"] = "0"
        return response

    async def index(_request):
        return static_response(web_root / "index.html")

    async def config(_request):
        try:
            kind, value = await gateway.control.request("GET_CONFIG")
            if kind != "CONFIG":
                raise RuntimeError(str(value))
            return web.json_response({"ok": True, "toml": value})
        except Exception as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=503)

    async def session(_request):
        # --listen is constant for the lifetime of this gateway process. The
        # browser requests it once per page/session instead of repeating it in
        # every live status poll.
        return web.json_response({"ok": True, "listen_ip": listen_ip, "version": meteoris_version()})

    async def status(_request):
        try:
            kind, value = await gateway.control.request("GET_STATUS")
            if kind != "STATUS":
                raise RuntimeError(str(value))
            value["gateway_psd_connected"] = gateway.psd_connected
            value["browser_psd_clients"] = len(gateway.websockets)
            return web.json_response({"ok": True, "status": value})
        except Exception as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=503)

    async def set_parameter(request):
        body = await request.json()
        parameter = str(body.get("parameter", ""))
        if parameter not in {"sdr.center_frequency", "sdr.gain"}:
            return web.json_response(
                {"ok": False, "error": "parameter is restart-required or read-only"}, status=400)
        try:
            value = float(body["value"])
            kind, response = await gateway.control.request(f"SET {parameter} {value:.17g}")
            if kind != "OK":
                return web.json_response({"ok": False, "error": response}, status=400)
            return web.json_response({"ok": True, "result": response})
        except Exception as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=503)

    async def websocket(request):
        ws = web.WebSocketResponse(heartbeat=15)
        await ws.prepare(request)
        gateway.websockets.add(ws)
        gateway._psd_wanted.set()
        try:
            async for _ in ws:
                pass
        finally:
            gateway.websockets.discard(ws)
            if not gateway.websockets:
                gateway._psd_wanted.clear()
        return ws

    app.router.add_get("/", index)
    app.router.add_get("/app.js", lambda r: static_response(web_root / "app.js"))
    app.router.add_get("/style.css", lambda r: static_response(web_root / "style.css"))
    app.router.add_get("/api/config", config)
    app.router.add_get("/api/session", session)
    app.router.add_get("/api/status", status)
    app.router.add_post("/api/set", set_parameter)
    app.router.add_get("/ws/psd", websocket)
    app.on_startup.append(gateway.start)
    app.on_cleanup.append(gateway.stop)
    return app


def main():
    parser = argparse.ArgumentParser(description="Meteoris live browser gateway")
    parser.add_argument("--version", action="version", version=f"%(prog)s {meteoris_version()}")
    parser.add_argument("--dsp-host", default="127.0.0.1")
    parser.add_argument("--psd-port", type=int, default=5510)
    parser.add_argument("--control-port", type=int, default=5511)
    parser.add_argument("--listen", default="127.0.0.1", help="HTTP bind address")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--web-root")
    args = parser.parse_args()

    gateway = Gateway(args.dsp_host, args.psd_port, args.control_port)
    app = make_app(gateway, find_web_root(args.web_root), args.listen)
    web.run_app(app, host=args.listen, port=args.http_port, print=lambda s: print(s))


if __name__ == "__main__":
    main()
