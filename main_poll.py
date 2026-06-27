"""SMS Relay - Polling Mode

Simple polling-based SMS reader for EC200A modem.
Runs once, reads all SMS from module, processes them, and exits.

Usage:
    python main_poll.py --info              # Show modem info
    python main_poll.py --list              # List all SMS
    python main_poll.py --unread            # List unread SMS only
    python main_poll.py --read              # Read and display all SMS
    python main_poll.py --read-unread       # Read and display unread SMS only
    python main_poll.py --read --delete     # Read and delete processed SMS
    python main_poll.py --read-unread --delete  # Read and delete unread SMS
    python main_poll.py --delete-all        # Delete all SMS
"""

from __future__ import annotations

import argparse
import signal
import sys
from typing import Optional, List, Dict, Any

from src.comm.serial import SerialComm, SerialConfig, list_ports
from src.comm.at import AtCommandHandler
from src.pdu.codec import decode_pdu
from src.sms.processor import SmsProcessor, SmsMessage


class SmsPoller:
    """Simple polling-based SMS reader.

    Reads SMS from module memory on demand, no event monitoring.
    """

    def __init__(self, port: str = "", baudrate: int = 115200):
        """Initialize SMS poller.

        Args:
            port: Serial port name (auto-detect if empty)
            baudrate: Baud rate (default: 115200 for EC200A)
        """
        self._port = port
        self._baudrate = baudrate
        self._comm: Optional[SerialComm] = None
        self._at: Optional[AtCommandHandler] = None
        self._processor = SmsProcessor(timeout=300.0)

    def connect(self) -> bool:
        """Connect to modem.

        Returns:
            True if successful
        """
        print(f"Connecting to modem on {self._port or 'auto-detect'}...")

        # Auto-detect port if not specified
        port = self._port
        if not port:
            ports = list_ports()
            if ports:
                port = ports[0]
                print(f"Auto-detected port: {port}")
            else:
                print("✗ No serial ports found!")
                return False

        # Configure and connect
        config = SerialConfig(port=port, baudrate=self._baudrate)
        self._comm = SerialComm(config)

        try:
            if not self._comm.connect():
                print("✗ Failed to open port")
                return False
            print("✓ Port opened")
        except Exception as e:
            print(f"✗ Failed to connect: {e}")
            return False

        # Create AT handler
        self._at = AtCommandHandler(self._comm)

        # Test modem
        if not self._at.test():
            print("✗ Modem not responding")
            return False
        print("✓ Modem responding")

        # Disable echo for cleaner output
        self._at.set_echo(enable=False)

        # Set PDU mode
        if not self._at.set_sms_format(pdu=True):
            print("✗ Failed to set PDU mode")
            return False

        # Configure for memory storage mode (mt=1)
        # This ensures SMS are stored to memory
        print("Configuring SMS storage mode...")
        if not self._at.set_sms_notification(mode=2, mt=1, bm=0, ds=1, bfr=0):
            print("⚠ Failed to configure CNMI (continuing anyway)")

        print("✓ Connected and configured")
        return True

    def disconnect(self) -> None:
        """Disconnect from modem."""
        if self._comm:
            self._comm.disconnect()
            print("Disconnected")

    def list_sms(self) -> List[Dict[str, Any]]:
        """List all SMS in memory.

        Returns:
            List of SMS info dictionaries with keys:
            - index: SMS index in memory
            - status: 'REC READ' or 'REC UNREAD'
            - sender: Phone number
            - timestamp: DateTime object
            - length: PDU length
        """
        print("\nListing all SMS...")

        sms_list = self._at.list_sms()
        if not sms_list:
            print("No SMS found")
            return []

        print(f"Found {len(sms_list)} SMS")
        return sms_list

    def list_unread_sms(self) -> List[Dict[str, Any]]:
        """List only unread SMS in memory.

        Returns:
            List of SMS info dictionaries (status = 0 or 'REC UNREAD')
        """
        print("\nListing unread SMS...")

        sms_list = self._at.list_sms()
        if not sms_list:
            print("No SMS found")
            return []

        # Filter only unread messages (status = 0 or 'REC UNREAD')
        unread_list = []
        for sms in sms_list:
            status = sms.get("status")
            # status can be int (0) or string ('REC UNREAD')
            if status == 0 or (isinstance(status, str) and "UNREAD" in status.upper()):
                unread_list.append(sms)

        if unread_list:
            print(f"Found {len(unread_list)} unread SMS (out of {len(sms_list)} total)")
        else:
            print("No unread SMS found")

        return unread_list

    def read_sms(self, index: int) -> Optional[SmsMessage]:
        """Read SMS from memory by index.

        Args:
            index: SMS index in memory

        Returns:
            SmsMessage object or None
        """
        print(f"\nReading SMS #{index}...")

        pdu = self._at.read_sms(index)
        if not pdu:
            print(f"✗ Failed to read SMS #{index}")
            return None

        print(f"  PDU: {pdu[:60]}...")

        # Decode PDU
        try:
            decoded = decode_pdu(pdu)
            sender = decoded.get("number", "Unknown")
            text = decoded.get("text", "")
            print(f"  Decoded: From {sender}, Text: {text[:50]}...")

            # Process through processor (handles concatenation)
            result = self._processor.process(pdu)

            if result and result.is_complete:
                return result
            else:
                print(f"  ⚠ Message incomplete (waiting for more parts)")
                return result

        except Exception as e:
            print(f"  ✗ Failed to decode PDU: {e}")
            import traceback

            traceback.print_exc()
            return None

    def read_all_sms(self) -> List[SmsMessage]:
        """Read all SMS from memory.

        Returns:
            List of SmsMessage objects
        """
        print("\n" + "=" * 60)
        print("Reading all SMS from memory")
        print("=" * 60)

        # List all SMS
        sms_list = self.list_sms()
        if not sms_list:
            return []

        # Read each SMS
        messages = []
        for sms_info in sms_list:
            index = sms_info.get("index")
            if index is None:
                continue

            message = self.read_sms(index)
            if message:
                messages.append(message)
                print(f"  ✓ SMS #{index}: {message.sender} - {message.text[:50]}...")

        print(f"\n✓ Read {len(messages)} SMS")
        return messages

    def read_unread_sms(self) -> List[SmsMessage]:
        """Read only unread SMS from memory.

        Returns:
            List of SmsMessage objects
        """
        print("\n" + "=" * 60)
        print("Reading unread SMS from memory")
        print("=" * 60)

        # List only unread SMS
        sms_list = self.list_unread_sms()
        if not sms_list:
            return []

        # Read each unread SMS
        messages = []
        for sms_info in sms_list:
            index = sms_info.get("index")
            if index is None:
                continue

            message = self.read_sms(index)
            if message:
                messages.append(message)
                print(f"  ✓ SMS #{index}: {message.sender} - {message.text[:50]}...")

        print(f"\n✓ Read {len(messages)} unread SMS")
        return messages

    def delete_sms(self, index: int) -> bool:
        """Delete SMS from memory by index.

        Args:
            index: SMS index in memory

        Returns:
            True if successful
        """
        print(f"Deleting SMS #{index}...")

        response = self._at.delete_sms(index)
        if response.success:
            print(f"  ✓ Deleted SMS #{index}")
            return True
        else:
            print(f"  ✗ Failed to delete SMS #{index}: {response.result_code}")
            return False

    def delete_all_sms(self) -> int:
        """Delete all SMS from memory.

        Returns:
            Number of SMS deleted
        """
        print("\nDeleting all SMS...")

        sms_list = self.list_sms()
        if not sms_list:
            return 0

        deleted = 0
        for sms_info in sms_list:
            index = sms_info.get("index")
            if index is not None and self.delete_sms(index):
                deleted += 1

        print(f"\n✓ Deleted {deleted}/{len(sms_list)} SMS")
        return deleted

    def get_modem_info(self) -> Dict[str, str]:
        """Get modem information.

        Returns:
            Dictionary with modem info
        """
        print("\nModem Information:")
        print("-" * 40)

        info = {}

        if model := self._at.get_model():
            info["model"] = model
            print(f"  Model: {model}")

        if imei := self._at.get_imei():
            info["imei"] = imei
            print(f"  IMEI: {imei}")

        if iccid := self._at.get_sim_ccid():
            info["iccid"] = iccid.replace("+CCID: ", "").strip()
            print(f"  ICCID: {info['iccid']}")

        # Get network info
        try:
            network = self._at.get_network_info()
            print(f"  Network: {'Registered' if network.registered else 'Not registered'}")
            print(f"  Operator: {network.operator or 'Unknown'}")
            print(f"  Signal: {network.rssi}/31 (BER: {network.ber})")
            info["network"] = {"registered": network.registered, "operator": network.operator, "rssi": network.rssi, "ber": network.ber}
        except Exception as e:
            print(f"  ⚠ Failed to get network info: {e}")

        return info

    def get_sms_status(self) -> Optional[Dict[str, int]]:
        """Get SMS memory status.

        Returns:
            Dictionary with used/total slots
        """
        try:
            status = self._at.get_sms_status()
            print(f"\nSMS Memory: {status.used}/{status.total} used")
            return {"used": status.used, "total": status.total}
        except Exception as e:
            print(f"⚠ Failed to get SMS status: {e}")
            return None


def process_message(message: SmsMessage) -> None:
    """Process received SMS message.

    Args:
        message: SMS message to process
    """
    print(f"\n{'=' * 60}")
    print(f"📩 SMS from {message.sender}")
    print(f"{'=' * 60}")
    print(f"Text: {message.text}")
    print(f"Time: {message.timestamp}")
    print(f"Type: {message.type.value}")
    print(f"Parts: {message.part_index}/{message.parts}")

    if message.smsc:
        print(f"SMSC: {message.smsc}")

    # TODO: Add your business logic here
    # Examples:
    # - Save to database
    # - Forward to API
    # - Parse commands
    # - Trigger actions


def main():
    """Main entry point for polling mode."""
    parser = argparse.ArgumentParser(
        description="SMS Relay - Polling Mode",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python main_poll.py --info              # Show modem info
  python main_poll.py --list              # List all SMS
  python main_poll.py --unread            # List unread SMS only
  python main_poll.py --read              # Read and display all SMS
  python main_poll.py --read-unread       # Read and display unread SMS only
  python main_poll.py --read --delete     # Read and delete processed SMS
  python main_poll.py --read-unread --delete  # Read and delete unread SMS
  python main_poll.py --delete-all        # Delete all SMS
        """,
    )

    parser.add_argument("--port", type=str, default="", help="Serial port (auto-detect if empty)")
    parser.add_argument("--baudrate", type=int, default=115200, help="Baud rate (default: 115200)")

    # Actions
    parser.add_argument("--info", action="store_true", help="Show modem information")
    parser.add_argument("--list", action="store_true", help="List all SMS in memory")
    parser.add_argument("--unread", action="store_true", help="List unread SMS only")
    parser.add_argument("--read", action="store_true", help="Read and display all SMS")
    parser.add_argument("--read-unread", action="store_true", help="Read and display unread SMS only")
    parser.add_argument("--delete", action="store_true", help="Delete SMS after reading (use with --read or --read-unread)")
    parser.add_argument("--delete-all", action="store_true", help="Delete all SMS")

    args = parser.parse_args()

    # Validate arguments
    if args.delete and not (args.read or args.read_unread):
        parser.error("--delete requires --read or --read-unread")

    action_count = sum([args.info, args.list, args.unread, args.read, args.read_unread, args.delete_all])
    if action_count == 0:
        parser.error("Please specify an action (--info, --list, --unread, --read, --read-unread, --delete-all)")
    if action_count > 1:
        parser.error("Only one action can be specified")

    # Create poller
    poller = SmsPoller(port=args.port, baudrate=args.baudrate)

    # Handle Ctrl+C gracefully
    def signal_handler(signum, frame):
        print("\n\nInterrupted...")
        poller.disconnect()
        sys.exit(1)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        # Connect
        if not poller.connect():
            sys.exit(1)

        # Execute requested action
        if args.info:
            poller.get_modem_info()
            poller.get_sms_status()

        elif args.list:
            sms_list = poller.list_sms()
            if sms_list:
                print(f"\n{'Index':<6} {'Status':<12} {'Sender':<20} {'Time'}")
                print("-" * 60)
                for sms in sms_list:
                    index = sms.get("index", "")
                    status = sms.get("status", "")

                    # Decode PDU to get sender and timestamp
                    sender = "Unknown"
                    timestamp_str = ""
                    pdu = sms.get("pdu", "")
                    if pdu:
                        try:
                            decoded = decode_pdu(pdu)
                            sender = decoded.get("number", "Unknown")
                            timestamp = decoded.get("time", "")
                            if timestamp:
                                # Parse timestamp string
                                try:
                                    from datetime import datetime

                                    # Handle different timestamp formats
                                    if isinstance(timestamp, str):
                                        # Try parsing ISO format
                                        ts = datetime.fromisoformat(timestamp.replace("+00:00", ""))
                                        timestamp_str = ts.strftime("%Y-%m-%d %H:%M")
                                    else:
                                        timestamp_str = str(timestamp)
                                except:
                                    timestamp_str = str(timestamp)[:16]
                        except:
                            sender = "Unknown"
                            timestamp_str = ""

                    # Format status
                    if status == 0:
                        status_str = "UNREAD"
                    elif status == 1:
                        status_str = "READ"
                    else:
                        status_str = str(status)

                    sender_display = sender[:20] if sender else "Unknown"
                    print(f"{index:<6} {status_str:<12} {sender_display:<20} {timestamp_str}")

        elif args.unread:
            sms_list = poller.list_unread_sms()
            if sms_list:
                print(f"\n{'Index':<6} {'Status':<12} {'Sender':<20} {'Time'}")
                print("-" * 60)
                for sms in sms_list:
                    index = sms.get("index", "")
                    status = sms.get("status", "")

                    # Decode PDU to get sender and timestamp
                    sender = "Unknown"
                    timestamp_str = ""
                    pdu = sms.get("pdu", "")
                    if pdu:
                        try:
                            decoded = decode_pdu(pdu)
                            sender = decoded.get("number", "Unknown")
                            timestamp = decoded.get("time", "")
                            if timestamp:
                                # Parse timestamp string
                                try:
                                    from datetime import datetime

                                    # Handle different timestamp formats
                                    if isinstance(timestamp, str):
                                        # Try parsing ISO format
                                        ts = datetime.fromisoformat(timestamp.replace("+00:00", ""))
                                        timestamp_str = ts.strftime("%Y-%m-%d %H:%M")
                                    else:
                                        timestamp_str = str(timestamp)
                                except:
                                    timestamp_str = str(timestamp)[:16]
                        except:
                            sender = "Unknown"
                            timestamp_str = ""

                    # Format status
                    if status == 0:
                        status_str = "UNREAD"
                    elif status == 1:
                        status_str = "READ"
                    else:
                        status_str = str(status)

                    sender_display = sender[:20] if sender else "Unknown"
                    print(f"{index:<6} {status_str:<12} {sender_display:<20} {timestamp_str}")

        elif args.read:
            messages = poller.read_all_sms()
            if messages:
                print(f"\n{'=' * 60}")
                print(f"Processing {len(messages)} messages")
                print(f"{'=' * 60}")

                for msg in messages:
                    process_message(msg)

                    if args.delete:
                        # Find original index (this is a limitation with concatenation)
                        # For now, skip delete for concatenated messages
                        print("  ⚠ Delete not supported for concatenated messages yet")

        elif args.read_unread:
            messages = poller.read_unread_sms()
            if messages:
                print(f"\n{'=' * 60}")
                print(f"Processing {len(messages)} unread messages")
                print(f"{'=' * 60}")

                for msg in messages:
                    process_message(msg)

                    if args.delete:
                        # Find original index (this is a limitation with concatenation)
                        # For now, skip delete for concatenated messages
                        print("  ⚠ Delete not supported for concatenated messages yet")

        elif args.delete_all:
            poller.delete_all_sms()

        # Disconnect
        print("\n" + "-" * 60)
        poller.disconnect()
        print("Done!")

    except Exception as e:
        print(f"\n✗ Error: {e}")
        import traceback

        traceback.print_exc()
        poller.disconnect()
        sys.exit(1)


if __name__ == "__main__":
    main()
