#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the golden protobuf vectors in test/fixtures/.

The codec in src/proto is hand-written, so it is checked against Google's
protobuf implementation rather than against its author. Run this after editing
anything under proto/:

    python3 -m venv /tmp/protovenv
    /tmp/protovenv/bin/pip install protobuf grpcio-tools
    /tmp/protovenv/bin/python -m grpc_tools.protoc -Iproto --python_out=/tmp/pb proto/*.proto
    /tmp/protovenv/bin/python tools/gen-vectors.py /tmp/pb > test/fixtures/protocomm-vectors.json

Nothing in the build depends on this; it is a check, not a code generator.
"""
import json
import sys

sys.path.insert(0, sys.argv[1])
import session_pb2, sec1_pb2, wifi_scan_pb2, wifi_config_pb2, wifi_constants_pb2, constants_pb2

V = {}

def put(name, msg):
    V[name] = msg.SerializeToString().hex()

# --- requests the client sends -------------------------------------------
CLIENT_PUB = bytes(range(32))
sd = session_pb2.SessionData()
sd.sec_ver = session_pb2.SecScheme1
sd.sec1.msg = sec1_pb2.Session_Command0
sd.sec1.sc0.client_pubkey = CLIENT_PUB
put("sessionCmd0", sd)

VERIFY = bytes(range(100, 132))
sd = session_pb2.SessionData()
sd.sec_ver = session_pb2.SecScheme1
sd.sec1.msg = sec1_pb2.Session_Command1
sd.sec1.sc1.client_verify_data = VERIFY
put("sessionCmd1", sd)

sp = wifi_scan_pb2.WiFiScanPayload()
sp.msg = wifi_scan_pb2.TypeCmdScanStart
sp.cmd_scan_start.blocking = True
sp.cmd_scan_start.passive = False
sp.cmd_scan_start.group_channels = 5
sp.cmd_scan_start.period_ms = 120
put("scanStart", sp)

sp = wifi_scan_pb2.WiFiScanPayload()
sp.msg = wifi_scan_pb2.TypeCmdScanStatus
sp.cmd_scan_status.SetInParent()
put("scanStatus", sp)

sp = wifi_scan_pb2.WiFiScanPayload()
sp.msg = wifi_scan_pb2.TypeCmdScanResult
sp.cmd_scan_result.start_index = 4
sp.cmd_scan_result.count = 4
put("scanResult_4_4", sp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeCmdSetConfig
cp.cmd_set_config.ssid = "Svermigo-ěščř".encode()
cp.cmd_set_config.passphrase = "p@ss word".encode()
put("setConfig_unicode", cp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeCmdApplyConfig
cp.cmd_apply_config.SetInParent()
put("applyConfig", cp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeCmdGetStatus
cp.cmd_get_status.SetInParent()
put("getStatus", cp)

# --- responses the device sends ------------------------------------------
sd = session_pb2.SessionData()
sd.sec_ver = session_pb2.SecScheme1
sd.sec1.msg = sec1_pb2.Session_Response0
sd.sec1.sr0.status = constants_pb2.Success
sd.sec1.sr0.device_pubkey = bytes(range(32, 64))
sd.sec1.sr0.device_random = bytes(range(16))
put("sessionResp0", sd)

sd = session_pb2.SessionData()
sd.sec_ver = session_pb2.SecScheme1
sd.sec1.msg = sec1_pb2.Session_Response1
sd.sec1.sr1.status = constants_pb2.Success
sd.sec1.sr1.device_verify_data = bytes(range(64, 96))
put("sessionResp1", sd)

sp = wifi_scan_pb2.WiFiScanPayload()
sp.msg = wifi_scan_pb2.TypeRespScanStatus
sp.resp_scan_status.scan_finished = True
sp.resp_scan_status.result_count = 5
put("respScanStatus", sp)

sp = wifi_scan_pb2.WiFiScanPayload()
sp.msg = wifi_scan_pb2.TypeRespScanResult
for ssid, ch, rssi, auth in [
    ("Svermigo", 6, -60, wifi_constants_pb2.WPA2_PSK),
    ("hostna-sit ěščř", 11, -91, wifi_constants_pb2.WPA_WPA2_PSK),
    ("open-ap", 1, -30, wifi_constants_pb2.Open),
]:
    e = sp.resp_scan_result.entries.add()
    e.ssid = ssid.encode(); e.channel = ch; e.rssi = rssi
    e.bssid = bytes([0xf4, 0x92, 0xbf, 0x5d, 0x8f, 0x70]); e.auth = auth
put("respScanResult", sp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeRespGetStatus
cp.resp_get_status.sta_state = wifi_constants_pb2.ConnectionFailed
cp.resp_get_status.fail_reason = wifi_constants_pb2.AuthError
put("statusAuthError", cp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeRespGetStatus
cp.resp_get_status.sta_state = wifi_constants_pb2.ConnectionFailed
cp.resp_get_status.fail_reason = wifi_constants_pb2.NetworkNotFound
put("statusNetworkNotFound", cp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeRespGetStatus
cp.resp_get_status.sta_state = wifi_constants_pb2.Connecting
cp.resp_get_status.attempt_failed.attempts_remaining = 3
put("statusAttemptsRemaining", cp)

cp = wifi_config_pb2.WiFiConfigPayload()
cp.msg = wifi_config_pb2.TypeRespGetStatus
cp.resp_get_status.sta_state = wifi_constants_pb2.Connected
cp.resp_get_status.connected.ip4_addr = "192.168.1.154"
cp.resp_get_status.connected.ssid = "Svermigo".encode()
cp.resp_get_status.connected.channel = 6
cp.resp_get_status.connected.auth_mode = wifi_constants_pb2.WPA2_PSK
put("statusConnected", cp)

print(json.dumps(V, indent=2, sort_keys=True))
