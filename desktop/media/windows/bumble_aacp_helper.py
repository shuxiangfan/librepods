#!/usr/bin/env python3
import asyncio
import json
import sys
from typing import Optional


def emit(event: str, **payload):
    msg = {"event": event}
    msg.update(payload)
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


class BumbleAacpBridge:
    def __init__(self):
        self.transport = None
        self.device = None
        self.connection = None
        self.aacp_channel = None
        self.reader_task: Optional[asyncio.Task] = None

    async def connect(self, bdaddr: str):
        try:
            from bumble.l2cap import ClassicChannelSpec
            from bumble.transport import open_transport
            from bumble.device import Device
            from bumble.host import Host
            from bumble.core import PhysicalTransport
            from bumble.keys import JsonKeyStore
            from bumble.pairing import PairingConfig, PairingDelegate
        except Exception as e:
            emit("error", code="missing_bumble", message=f"Bumble import failed: {e}")
            return

        try:
            self.transport = await open_transport("usb:0")
            self.device = Device(host=Host(controller_source=self.transport.source, controller_sink=self.transport.sink))
            self.device.classic_enabled = True
            self.device.le_enabled = False
            self.device.keystore = JsonKeyStore.from_device(self.device, "./keys.json")
            self.device.pairing_config_factory = lambda conn: PairingConfig(
                sc=True, mitm=False, bonding=True,
                delegate=PairingDelegate(io_capability=PairingDelegate.NO_OUTPUT_NO_INPUT)
            )
            await self.device.power_on()

            self.connection = await self.device.connect(bdaddr, PhysicalTransport.BR_EDR)
            await self.connection.authenticate()
            if not self.connection.is_encrypted:
                await self.connection.encrypt()

            spec = ClassicChannelSpec(psm=0x1001, mtu=2048)
            self.aacp_channel = await self.connection.create_l2cap_channel(spec=spec)
            self.aacp_channel.sink = self._on_aacp_pdu
            self.reader_task = None
            emit("connected")
        except Exception as e:
            emit("error", code="connect_failed", message=f"Bumble connect failed: {e}")
            await self.disconnect()

    def _on_aacp_pdu(self, pdu: bytes):
        try:
            emit("aacp_pdu", hex=bytes(pdu).hex())
        except Exception as e:
            emit("error", code="io_error", message=f"Failed to forward AACP PDU: {e}")

    async def send_aacp(self, hex_data: str):
        if not self.aacp_channel:
            emit("error", code="not_connected", message="AACP channel not connected")
            return
        try:
            self.aacp_channel.send_pdu(bytes.fromhex(hex_data))
        except Exception as e:
            emit("error", code="send_failed", message=f"AACP send failed: {e}")

    async def disconnect(self):
        try:
            if self.aacp_channel:
                try:
                    self.aacp_channel.disconnect()
                except Exception:
                    pass
                self.aacp_channel = None
            if self.connection:
                try:
                    await self.connection.disconnect()
                except Exception:
                    pass
                self.connection = None
            if self.transport:
                try:
                    await self.transport.close()
                except Exception:
                    pass
                self.transport = None
            if self.device:
                self.device = None
        finally:
            emit("disconnected")


async def stdin_loop(bridge: BumbleAacpBridge):
    loop = asyncio.get_running_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            emit("error", code="bad_json", message=f"Invalid JSON command: {e}")
            continue

        cmd = msg.get("cmd")
        if cmd == "connect":
            await bridge.connect(msg.get("bdaddr", ""))
        elif cmd == "aacp_send":
            await bridge.send_aacp(msg.get("hex", ""))
        elif cmd == "disconnect":
            await bridge.disconnect()
        elif cmd == "shutdown":
            await bridge.disconnect()
            break
        else:
            emit("error", code="unknown_cmd", message=f"Unknown command: {cmd}")


async def main():
    bridge = BumbleAacpBridge()
    try:
        await stdin_loop(bridge)
    finally:
        await bridge.disconnect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
