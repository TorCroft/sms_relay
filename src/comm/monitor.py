"""Serial Port Event Monitor with Callbacks

Implements event-driven serial port communication using background thread
and callbacks. Eliminates the need for polling.

Example:
    >>> monitor = SerialMonitor(config)
    >>>
    >>> def on_data(data):
    ...     print(f"Received: {data}")
    >>>
    >>> monitor.on_data_receive = on_data
    >>> monitor.start()
"""

from __future__ import annotations

import re
import threading
import time
from dataclasses import dataclass, field
from typing import Optional, Callable, List, Dict, Any
from enum import Enum

try:
    from serial import SerialException
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


class SerialEventType(Enum):
    """Serial event types"""
    DATA_RECEIVED = "data_received"
    DATA_SENT = "data_sent"
    CONNECTED = "connected"
    DISCONNECTED = "disconnected"
    ERROR = "error"
    SMS_NOTIFICATION = "sms_notification"
    LINE_RECEIVED = "line_received"


@dataclass
class SerialEvent:
    """Serial port event.

    Attributes:
        type: Event type
        data: Associated data (bytes, str, or dict)
        timestamp: Event timestamp
    """
    type: SerialEventType
    data: Any = None
    timestamp: float = field(default_factory=time.time)


class SerialMonitor:
    """Event-driven serial port monitor.

    Uses background thread to read serial port and trigger callbacks
    when events occur, eliminating need for polling.

    Example:
        >>> monitor = SerialMonitor(config)
        >>>
        >>> @monitor.on_line
        >>> def handle_line(line: str):
        ...     if line.startswith('+CMTI'):
        ...         print(f"New SMS: {line}")
        >>>
        >>> monitor.start()
    """

    def __init__(self, serial_config):
        """Initialize serial monitor.

        Args:
            serial_config: SerialConfig object
        """
        if not SERIAL_AVAILABLE:
            raise ImportError("pyserial required")

        from .serial import SerialConfig
        # Import SerialComm only for type hint, use string to avoid circular import
        from typing import TYPE_CHECKING
        if TYPE_CHECKING:
            from .serial import SerialComm

        self._config = serial_config

        self._comm: "Optional[SerialComm]" = None
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.RLock()

        # Event callbacks
        self._callbacks: Dict[SerialEventType, List[Callable]] = {
            SerialEventType.DATA_RECEIVED: [],
            SerialEventType.LINE_RECEIVED: [],
            SerialEventType.SMS_NOTIFICATION: [],
            SerialEventType.CONNECTED: [],
            SerialEventType.DISCONNECTED: [],
            SerialEventType.ERROR: [],
        }

    @property
    def is_running(self) -> bool:
        """Check if monitor is running.

        Returns:
            True if monitoring thread is active
        """
        return self._running and self._thread is not None and self._thread.is_alive()

    def start(self) -> bool:
        """Start monitoring serial port in background thread.

        Returns:
            True if started successfully

        Example:
            >>> monitor = SerialMonitor(config)
            >>> monitor.start()
            >>> # Monitor runs in background...
            >>> monitor.stop()
        """
        with self._lock:
            if self.is_running:
                return True

            try:
                from .serial import SerialComm
                self._comm = SerialComm(self._config)
                self._comm.connect()

                self._running = True
                self._thread = threading.Thread(target=self._monitor_loop, daemon=True)
                self._thread.start()

                # Trigger connected event
                self._trigger_event(SerialEventType.CONNECTED)

                return True

            except Exception as e:
                self._trigger_event(SerialEventType.ERROR, data=e)
                return False

    def stop(self) -> None:
        """Stop monitoring serial port.

        Waits for monitoring thread to finish (with timeout).
        """
        with self._lock:
            if not self._running:
                return

            self._running = False

        # Wait for thread to finish
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None

        if self._comm:
            self._comm.disconnect()
            self._trigger_event(SerialEventType.DISCONNECTED)

    def _monitor_loop(self) -> None:
        """Background monitoring loop."""
        buffer = bytearray()

        try:
            while self._running:
                # Read data
                data = self._comm.read(1024, timeout=0.5)
                if not data:
                    continue

                # Add to buffer
                buffer.extend(data)
                buffer_str = buffer.decode('ascii', errors='replace')

                # Check for line endings
                while b'\r\n' in buffer or b'\n' in buffer:
                    if b'\r\n' in buffer:
                        parts = buffer.split(b'\r\n', 1)
                        line = parts[0]
                        buffer = bytearray(parts[1]) if len(parts) > 1 else bytearray()
                    else:
                        parts = buffer.split(b'\n', 1)
                        line = parts[0]
                        buffer = bytearray(parts[1]) if len(parts) > 1 else bytearray()

                    line_str = line.decode('ascii', errors='replace').strip()
                    if line_str:
                        self._trigger_event(SerialEventType.LINE_RECEIVED, data=line_str)

                # Check for SMS notifications
                self._check_sms_notifications(buffer_str)

                # Trigger data received event
                if data:
                    self._trigger_event(SerialEventType.DATA_RECEIVED, data=data)

                # Keep buffer reasonable size
                if len(buffer) > 4096:
                    buffer = bytearray()

        except Exception as e:
            self._trigger_event(SerialEventType.ERROR, data=e)

    def _check_sms_notifications(self, buffer_str: str) -> None:
        """Check for SMS notifications in buffer.

        Args:
            buffer_str: String buffer to check
        """
        # Check +CMTI: "SM",<index> - notification with index
        cmti_match = re.search(r'\+CMTI:\s*"([^"]*)",\s*(\d+)', buffer_str)
        if cmti_match:
            event_data = {
                'memory': cmti_match.group(1),
                'index': int(cmti_match.group(2))
            }
            self._trigger_event(SerialEventType.SMS_NOTIFICATION, data=event_data)

        # Check for +CMT: <length><CR><LF><pdu> - direct PDU
        # The PDU may span multiple lines, so we need to capture everything after the line ending
        cmt_match = re.search(r'\+CMT:\s*(\d+)(\r\n|\n)([0-9A-Fa-f]+)', buffer_str, re.IGNORECASE)
        if cmt_match:
            pdu_len = int(cmt_match.group(1))
            pdu = cmt_match.group(3)
            # Remove any whitespace/non-hex chars from PDU
            pdu_clean = re.sub(r'[^0-9A-Fa-f]', '', pdu)
            if len(pdu_clean) == pdu_len * 2:  # Validate PDU length
                event_data = {'pdu': pdu_clean, 'length': pdu_len}
                self._trigger_event(SMS_NOTIFICATION, data=event_data)

    def _trigger_event(self, event_type: SerialEventType, data: Any = None) -> None:
        """Trigger all callbacks registered for event type.

        Args:
            event_type: Event type
            data: Event data
        """
        callbacks = self._callbacks.get(event_type, [])

        for callback in callbacks:
            try:
                event = SerialEvent(type=event_type, data=data)
                callback(event)
            except Exception as e:
                # Callback error shouldn't crash monitor
                pass

    # Decorator for registering callbacks
    def on_event(self, event_type: SerialEventType):
        """Decorator for registering event callbacks.

        Args:
            event_type: Event type to listen for

        Example:
            >>> monitor = SerialMonitor(config)
            >>>
            >>> @monitor.on_event(SerialEventType.LINE_RECEIVED)
            >>> def handle_line(event: SerialEvent):
            ...     if event.data.startswith('+CMTI'):
            ...         print(f"New SMS: {event.data}")
            >>>
            >>> monitor.start()
        """
        def decorator(callback: Callable) -> Callable:
            self._callbacks[event_type].append(callback)
            return callback
        return decorator

    # Convenience decorators for common events
    def on_data(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for data received events."""
        return self.on_event(SerialEventType.DATA_RECEIVED)(callback)

    def on_line(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for line received events."""
        return self.on_event(SerialEventType.LINE_RECEIVED)(callback)

    def on_sms(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for SMS notification events."""
        return self.on_event(SerialEventType.SMS_NOTIFICATION)(callback)

    def on_error(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for error events."""
        return self.on_event(SerialEventType.ERROR)(callback)

    def on_connected(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for connection events."""
        return self.on_event(SerialEventType.CONNECTED)(callback)

    def on_disconnected(self, callback: Callable[[SerialEvent], None]) -> Callable:
        """Register callback for disconnection events."""
        return self.on_event(SerialEventType.DISCONNECTED)(callback)


class SmsEventMonitor:
    """High-level SMS event monitor with automatic PDU decoding.

    Combines SerialMonitor with SMS processor for complete event-driven
    SMS handling.

    Example:
        >>> monitor = SmsEventMonitor(config)
        >>>
        >>> @monitor.on_sms_complete
        >>> def handle_sms(message: SmsMessage):
        ...     print(f"SMS from {message.sender}: {message.text}")
        >>>
        >>> monitor.start()
    """

    def __init__(self, serial_config, pdu_timeout: float = 300.0, at_handler=None, existing_comm=None):
        """Initialize SMS event monitor.

        Args:
            serial_config: SerialConfig object
            pdu_timeout: Timeout for SMS concatenation
            at_handler: Optional AtCommandHandler instance for reading SMS
            existing_comm: Optional existing SerialComm instance to share
        """
        from comm.serial import SerialConfig
        from sms.processor import SmsProcessor

        self._config = serial_config
        self._pdu_timeout = pdu_timeout
        self._at_handler = at_handler  # Store AT handler if provided
        self._existing_comm = existing_comm  # Store existing comm if provided

        # Initialize components
        if existing_comm:
            # Use existing SerialComm instance - don't create new connection
            self._monitor = SerialMonitor(serial_config)
            self._monitor._comm = existing_comm
            # Don't call connect() - already connected
        else:
            # Create new connection
            self._monitor = SerialMonitor(serial_config)

        self._processor = SmsProcessor(timeout=pdu_timeout)

        # SMS-specific callbacks
        self._sms_callbacks: List[Callable] = []

    def start(self) -> bool:
        """Start SMS event monitoring.

        Returns:
            True if started successfully
        """
        # Register SMS handler with monitor
        self._monitor.on_sms(self._handle_sms_notification)

        # Start underlying monitor
        if self._existing_comm:
            # Already connected, just start monitoring thread
            self._monitor._running = True
            self._monitor._thread = threading.Thread(target=self._monitor._monitor_loop, daemon=True)
            self._monitor._thread.start()
            return True
        else:
            # Need to establish connection first
            return self._monitor.start()

    def stop(self) -> None:
        """Stop SMS event monitoring."""
        self._monitor.stop()

    @property
    def is_running(self) -> bool:
        """Check if monitor is running."""
        return self._monitor.is_running

    def _handle_sms_notification(self, event: SerialEvent) -> None:
        """Handle SMS notification from serial monitor.

        Args:
            event: Serial event with SMS data
        """
        event_data = event.data

        # Handle +CMTI notification (need to read PDU)
        if isinstance(event_data, dict) and 'memory' in event_data:
            mem = event_data['memory']
            index = event_data['index']

            # Read PDU
            pdu = self._read_sms_pdu(mem, index)
            if pdu:
                self._process_pdu(pdu)

        # Handle +CMT notification (direct PDU)
        elif isinstance(event_data, dict) and 'pdu' in event_data:
            self._process_pdu(event_data['pdu'])

    def _read_sms_pdu(self, mem: str, index: int) -> Optional[str]:
        """Read SMS PDU from memory.

        Args:
            mem: Memory name (e.g., "SM")
            index: SMS index

        Returns:
            PDU hex string or None
        """
        try:
            if self._at_handler:
                return self._at_handler.read_sms(index)
            return None
        except Exception:
            return None

    def _process_pdu(self, pdu: str) -> None:
        """Process PDU through SMS processor.

        Args:
            pdu: PDU hex string
        """
        try:
            result = self._processor.process(pdu)

            if result and result.is_complete:
                # Trigger complete SMS callbacks
                for callback in self._sms_callbacks:
                    try:
                        callback(result)
                    except Exception:
                        pass

        except Exception:
            pass

    def on_sms_complete(self, callback: Callable) -> Callable:
        """Register callback for complete SMS messages.

        Args:
            callback: Function receiving SmsMessage object

        Returns:
            The decorated function

        Example:
            >>> @monitor.on_sms_complete
            >>> def handle_sms(message: SmsMessage):
            ...     print(f"SMS from {message.sender}: {message.text}")
        """
        def wrapper(func: Callable) -> Callable:
            self._sms_callbacks.append(func)
            return wrapper
        return wrapper

    @property
    def stats(self) -> Dict[str, Any]:
        """Get statistics.

        Returns:
            Statistics dictionary
        """
        return {
            'running': self.is_running,
            'pending_messages': len(self._processor.get_pending()),
            'serial_stats': self._monitor._comm.stats if self._monitor._comm else None
        }


# =============================================================================
# Convenience Functions
# =============================================================================

def create_event_monitor(serial_config, enable_sms: bool = True):
    """Create event-driven serial monitor.

    Args:
        serial_config: SerialConfig object
        enable_sms: True for SmsEventMonitor, False for SerialMonitor

    Returns:
        SerialMonitor or SmsEventMonitor instance

    Example:
        >>> from .serial import SerialConfig
        >>> config = SerialConfig(port='COM3', baudrate=115200)
        >>>
        >>> # Simple serial monitoring
        >>> monitor = create_event_monitor(config, enable_sms=False)
        >>>
        >>> # Full SMS event monitoring
        >>> sms_monitor = create_event_monitor(config, enable_sms=True)
    """
    if enable_sms:
        return SmsEventMonitor(serial_config)
    return SerialMonitor(serial_config)
