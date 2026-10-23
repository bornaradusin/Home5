#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small stand-in for Home Assistant, for tests/test_home5.c: the login flow,
tokens and the WebSocket API, over HTTP or HTTPS.

    tests/fake_ha.py PORT READY_FILE [CERT KEY]

Writes READY_FILE once it listens; serves until killed. Accounts: kevin/secret,
and mfa/secret with the two-factor code 123456. A long-lived token: the one in
LONG_LIVED. Some scripts make it do things the tests need:
script.notify_test (a persistent notification), script.rename_kitchen (an area
renamed: the registry-updated event), script.drop (the connection closed).
"""
import base64
import copy
import hashlib
import http.server
import json
import socket
import ssl
import struct
import sys
import threading
from pathlib import Path

LONG_LIVED = "LLT-0123456789abcdefghijklmnopqrstuvwxyz"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CLIENT_ID = "https://github.com/bornaradusin/Home5"

lock = threading.Lock()
flows = {}           # flow id -> {"user": ..., "step": "init" | "mfa"}
codes = {}           # one-time code -> user
access_tokens = {LONG_LIVED}
refresh_tokens = {}  # refresh -> user
revoked = []
calls = []
counter = [0]
sockets = []

AREAS = [
    {"area_id": "living_room", "name": "Living room"},
    {"area_id": "kitchen", "name": "Kitchen"},
    {"area_id": "bedroom", "name": "Bedroom"},
    {"area_id": "garage", "name": "Garage"},
]
DEVICES = [
    {"id": "dev1", "area_id": "living_room", "name": "Ceiling"},
    {"id": "dev2", "area_id": "kitchen", "name": "Kettle"},
    {"id": "dev3", "area_id": None, "name": "Front door"},
]


def reg(entity_id, area=None, device=None, name=None, hidden=None, disabled=None, category=None):
    return {"entity_id": entity_id, "area_id": area, "device_id": device, "name": name, "original_name": None,
            "hidden_by": hidden, "disabled_by": disabled, "entity_category": category, "platform": "test"}


ENTITIES = [
    reg("light.ceiling", device="dev1"),
    reg("light.lamp", area="living_room"),
    reg("switch.kettle", device="dev2"),
    reg("climate.living", area="living_room"),
    reg("cover.blinds", area="living_room"),
    reg("sensor.living_temperature", area="living_room"),
    reg("sensor.living_humidity", area="living_room"),
    reg("sensor.living_rssi", area="living_room", category="diagnostic"),
    reg("light.hidden", area="living_room", hidden="user"),
    reg("binary_sensor.front_door", device="dev3"),
    reg("scene.movie", area="living_room"),
    reg("lock.front_door", device="dev3"),
    reg("light.bedroom", area="bedroom", name="Bedside"),
    reg("script.notify_test"),
    reg("script.rename_kitchen"),
    reg("script.drop"),
]


def state(entity_id, value, **attributes):
    return {"entity_id": entity_id, "state": value, "attributes": attributes,
            "last_changed": "2026-10-23T06:00:00+00:00", "context": {"id": "x"}}


STATES = {s["entity_id"]: s for s in [
    state("light.ceiling", "on", friendly_name="Living room Ceiling light", brightness=204,
          supported_color_modes=["color_temp"], color_mode="color_temp", color_temp_kelvin=2700,
          min_color_temp_kelvin=2200, max_color_temp_kelvin=6500),
    state("light.lamp", "off", friendly_name="Floor lamp", supported_color_modes=["brightness"], brightness=None),
    state("switch.kettle", "off", friendly_name="Kettle"),
    state("climate.living", "heat", friendly_name="Thermostat", current_temperature=21.4, temperature=22,
          target_temp_step=0.5, min_temp=7, max_temp=30, hvac_modes=["heat", "cool", "heat_cool", "off"],
          preset_modes=["home", "away", "sleep"], preset_mode="home", hvac_action="heating"),
    state("cover.blinds", "open", friendly_name="Blinds", current_position=60, supported_features=15),
    state("sensor.living_temperature", "21.43", friendly_name="Living room Temperature",
          unit_of_measurement="°C", device_class="temperature"),
    state("sensor.living_humidity", "48", friendly_name="Living room Humidity", unit_of_measurement="%",
          device_class="humidity"),
    state("sensor.living_rssi", "-60", friendly_name="Signal", unit_of_measurement="dBm"),
    state("light.hidden", "on", friendly_name="Hidden light", supported_color_modes=["onoff"]),
    state("binary_sensor.front_door", "off", friendly_name="Front door", device_class="door"),
    state("scene.movie", "2026-10-22T20:00:00+00:00", friendly_name="Movie night"),
    state("lock.front_door", "locked", friendly_name="Front door lock"),
    state("light.bedroom", "on", friendly_name="Bedside", brightness=128, supported_color_modes=["hs", "color_temp"],
          rgb_color=[255, 154, 98], color_mode="hs"),
    state("script.notify_test", "off", friendly_name="Notify test"),
    state("script.rename_kitchen", "off", friendly_name="Rename kitchen"),
    state("script.drop", "off", friendly_name="Drop"),
    state("sun.sun", "above_horizon", friendly_name="Sun"),
]}


def new_id(prefix):
    with lock:
        counter[0] += 1
        return f"{prefix}{counter[0]}"


class Socket:
    """A WebSocket on an upgraded connection (server side: frames unmasked)."""

    def __init__(self, raw):
        self.raw = raw
        self.send_lock = threading.Lock()

    def recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.raw.recv(n - len(data))
            if not chunk:
                raise ConnectionError
            data += chunk
        return data

    def receive(self):
        while True:
            head = self.recv_exact(2)
            opcode, masked, length = head[0] & 15, head[1] >> 7, head[1] & 127
            if length == 126:
                length = struct.unpack(">H", self.recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self.recv_exact(8))[0]
            mask = self.recv_exact(4) if masked else b"\0\0\0\0"
            if not masked:
                raise ConnectionError("client frames must be masked")
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(self.recv_exact(length)))
            if opcode == 8:
                raise ConnectionError
            if opcode == 9:
                self.frame(10, payload)
                continue
            if opcode == 1:
                return json.loads(payload)

    def frame(self, opcode, payload):
        if len(payload) < 126:
            head = struct.pack(">BB", 0x80 | opcode, len(payload))
        elif len(payload) < 65536:
            head = struct.pack(">BBH", 0x80 | opcode, 126, len(payload))
        else:
            head = struct.pack(">BBQ", 0x80 | opcode, 127, len(payload))
        with self.send_lock:
            self.raw.sendall(head + payload)

    def send(self, message, fragments=1):
        data = json.dumps(message).encode()
        if fragments == 1:
            self.frame(1, data)
            return
        # The same message in pieces (text, then continuations).
        size = len(data) // fragments + 1
        pieces = [data[i:i + size] for i in range(0, len(data), size)]
        with self.send_lock:
            for k, piece in enumerate(pieces):
                opcode = 1 if k == 0 else 0
                fin = 0x80 if k == len(pieces) - 1 else 0
                self.raw.sendall(struct.pack(">BB", fin | opcode, 126 if len(piece) >= 126 else len(piece)) +
                                 (struct.pack(">H", len(piece)) if len(piece) >= 126 else b"") + piece)


def apply_call(domain, service, entity_id, data):
    s = STATES.get(entity_id)
    if not s or not entity_id.startswith(domain + "."):
        return None
    a = s["attributes"]
    if service in ("turn_on", "turn_off", "toggle") and domain in ("light", "switch", "fan", "input_boolean"):
        on = service == "turn_on" or (service == "toggle" and s["state"] != "on")
        s["state"] = "on" if on else "off"
        if domain == "light":
            if "brightness_pct" in data:
                a["brightness"] = round(data["brightness_pct"] * 255 / 100)
            if "rgb_color" in data:
                a["rgb_color"] = data["rgb_color"]
            if "color_temp_kelvin" in data:
                a["color_temp_kelvin"] = data["color_temp_kelvin"]
            if not on:
                a["brightness"] = None
            elif a.get("brightness") is None and a.get("supported_color_modes") != ["onoff"]:
                a["brightness"] = 255
    elif domain == "climate" and service == "set_temperature":
        a["temperature"] = data["temperature"]
    elif domain == "climate" and service == "set_hvac_mode":
        s["state"] = data["hvac_mode"]
    elif domain == "climate" and service == "set_preset_mode":
        a["preset_mode"] = data["preset_mode"]
    elif domain == "cover" and service == "set_cover_position":
        a["current_position"] = data["position"]
        s["state"] = "closed" if data["position"] == 0 else "open"
    elif domain == "cover" and service == "toggle":
        s["state"] = "closed" if s["state"] == "open" else "open"
        a["current_position"] = 0 if s["state"] == "closed" else 100
    elif domain == "lock" and service in ("lock", "unlock"):
        s["state"] = "locked" if service == "lock" else "unlocked"
    elif domain == "scene" and service == "turn_on":
        s["state"] = "2026-10-23T09:00:00+00:00"   # when it was last activated, as HA keeps it
    elif domain == "script" and service == "turn_on":
        pass
    else:
        return None
    return s


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def answer(self, status, body):
        data = json.dumps(body).encode() if not isinstance(body, bytes) else body
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        # Chunked sometimes, to cover both.
        if self.path.endswith("/auth/token"):
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(data), 20):
                piece = data[i:i + 20]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)

    def body(self):
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length).decode()

    def do_POST(self):
        text = self.body()
        if self.path == "/auth/login_flow":
            data = json.loads(text)
            if data.get("client_id") != CLIENT_ID or data.get("handler") != ["homeassistant", None] or \
                    not data.get("redirect_uri", "").startswith(CLIENT_ID):
                return self.answer(400, {"message": "Invalid client id or redirect uri"})
            flow = new_id("flow")
            flows[flow] = {"step": "init"}
            return self.answer(200, {"type": "form", "flow_id": flow, "handler": ["homeassistant", None],
                                     "step_id": "init", "data_schema": [], "errors": {}})
        if self.path.startswith("/auth/login_flow/"):
            flow = self.path.rsplit("/", 1)[1]
            data = json.loads(text)
            f = flows.get(flow)
            if not f or data.get("client_id") != CLIENT_ID:
                return self.answer(404, {"message": "Invalid flow specified"})
            if f["step"] == "init":
                user, password = data.get("username"), data.get("password")
                if password != "secret" or user not in ("kevin", "mfa"):
                    return self.answer(200, {"type": "form", "flow_id": flow, "step_id": "init",
                                             "errors": {"base": "invalid_auth"}})
                f["user"] = user
                if user == "mfa":
                    f["step"] = "mfa"
                    return self.answer(200, {"type": "form", "flow_id": flow, "step_id": "mfa", "errors": {}})
            elif f["step"] == "mfa":
                if data.get("code") != "123456":
                    return self.answer(200, {"type": "form", "flow_id": flow, "step_id": "mfa",
                                             "errors": {"base": "invalid_code"}})
            code = new_id("code")
            codes[code] = f["user"]
            del flows[flow]
            return self.answer(200, {"type": "create_entry", "flow_id": flow, "result": code, "version": 1})
        if self.path == "/auth/token":
            form = dict(p.split("=", 1) for p in text.split("&") if "=" in p)
            from urllib.parse import unquote
            form = {k: unquote(v) for k, v in form.items()}
            if form.get("client_id") != CLIENT_ID:
                return self.answer(400, {"error": "invalid_request"})
            if form.get("grant_type") == "authorization_code" and form.get("code") in codes:
                user = codes.pop(form["code"])
                refresh = new_id("RT-")
                refresh_tokens[refresh] = user
                access = new_id("AT-")
                access_tokens.add(access)
                return self.answer(200, {"access_token": access, "expires_in": 1800, "refresh_token": refresh,
                                         "token_type": "Bearer"})
            if form.get("grant_type") == "refresh_token" and form.get("refresh_token") in refresh_tokens:
                access = new_id("AT-")
                access_tokens.add(access)
                return self.answer(200, {"access_token": access, "expires_in": 1800, "token_type": "Bearer"})
            return self.answer(400, {"error": "invalid_grant"})
        if self.path == "/auth/revoke":
            from urllib.parse import unquote
            token = unquote(text.split("=", 1)[1]) if "=" in text else ""
            revoked.append(token)
            refresh_tokens.pop(token, None)
            return self.answer(200, b"")
        return self.answer(404, {"message": "Not found"})

    def do_GET(self):
        if self.path == "/test/calls":
            return self.answer(200, {"calls": calls, "revoked": revoked})
        if self.path != "/api/websocket" or self.headers.get("Upgrade", "").lower() != "websocket":
            return self.answer(404, {"message": "Not found"})
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        ws = Socket(self.connection)
        sockets.append(ws)
        try:
            self.serve_ws(ws)
        except (ConnectionError, OSError, ValueError):
            pass

    def serve_ws(self, ws):
        ws.send({"type": "auth_required", "ha_version": "2026.10.0"})
        auth = ws.receive()
        if auth.get("type") != "auth" or auth.get("access_token") not in access_tokens:
            ws.send({"type": "auth_invalid", "message": "Invalid access token or password"})
            return
        ws.send({"type": "auth_ok", "ha_version": "2026.10.0"})
        subscriptions = {}
        while True:
            m = ws.receive()
            kind, mid = m.get("type"), m.get("id")
            if kind == "config/area_registry/list":
                ws.send({"id": mid, "type": "result", "success": True, "result": AREAS})
            elif kind == "config/device_registry/list":
                ws.send({"id": mid, "type": "result", "success": True, "result": DEVICES})
            elif kind == "config/entity_registry/list":
                # Over 64 KiB in one frame: the 64-bit length.
                fillers = [dict(reg(f"sensor.filler_{i}", area="garage"), options={"padding": "x" * 600})
                           for i in range(120)]
                ws.send({"id": mid, "type": "result", "success": True, "result": ENTITIES + fillers})
            elif kind == "get_states":
                # In fragments.
                ws.send({"id": mid, "type": "result", "success": True, "result": list(STATES.values()) +
                         [state(f"sensor.filler_{i}", "1", friendly_name=f"Filler {i}", padding="x" * 600)
                          for i in range(120)]}, fragments=3)
            elif kind == "get_config":
                ws.send({"id": mid, "type": "result", "success": True,
                         "result": {"unit_system": {"temperature": "°C"}, "version": "2026.10.0"}})
            elif kind == "subscribe_events":
                subscriptions[m.get("event_type")] = mid
                ws.send({"id": mid, "type": "result", "success": True, "result": None})
            elif kind == "persistent_notification/subscribe":
                subscriptions["notes"] = mid
                ws.send({"id": mid, "type": "result", "success": True, "result": None})
                ws.send({"id": mid, "type": "event", "event": {"type": "current", "notifications": {
                    "old": {"notification_id": "old", "title": "Old", "message": "Already there"}}}})
            elif kind == "ping":
                ws.send({"id": mid, "type": "pong"})
            elif kind == "call_service":
                entity = m.get("target", {}).get("entity_id", "")
                calls.append([m["domain"], m["service"], entity, m.get("service_data", {})])
                if entity == "script.drop":
                    ws.raw.shutdown(socket.SHUT_RDWR)
                    return
                old = copy.deepcopy(STATES.get(entity))
                new = apply_call(m["domain"], m["service"], entity, m.get("service_data", {}))
                if new is None:
                    ws.send({"id": mid, "type": "result", "success": False,
                             "error": {"code": "not_found", "message": f"Service {m['domain']}.{m['service']} not found."}})
                    continue
                ws.send({"id": mid, "type": "result", "success": True, "result": {"context": {"id": "c"}}})
                if "state_changed" in subscriptions:
                    ws.send({"id": subscriptions["state_changed"], "type": "event", "event": {
                        "event_type": "state_changed",
                        "data": {"entity_id": entity, "old_state": old, "new_state": new}}})
                if entity == "script.notify_test" and "notes" in subscriptions:
                    ws.send({"id": subscriptions["notes"], "type": "event", "event": {"type": "added", "notifications": {
                        "n1": {"notification_id": "n1", "title": "Doorbell", "message": "Someone is at the door"}}}})
                if entity == "script.rename_kitchen" and "area_registry_updated" in subscriptions:
                    AREAS[1]["name"] = "Cookhouse"
                    ws.send({"id": subscriptions["area_registry_updated"], "type": "event", "event": {
                        "event_type": "area_registry_updated", "data": {"action": "update", "area_id": "kitchen"}}})
            else:
                ws.send({"id": mid, "type": "result", "success": False,
                         "error": {"code": "unknown_command", "message": "Unknown command."}})


def main():
    port, ready = int(sys.argv[1]), Path(sys.argv[2])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    if len(sys.argv) > 4:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(sys.argv[3], sys.argv[4])
        server.socket = context.wrap_socket(server.socket, server_side=True)
    ready.write_text("ready")
    server.serve_forever()


if __name__ == "__main__":
    main()
