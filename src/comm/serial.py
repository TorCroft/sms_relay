"""Serial Communication Module for EC200A

Modern serial port communication module specifically designed for
Quectel EC200A cellular modem and compatible devices.

Features:
- Thread-safe operations
- Automatic reconnection
- Context manager support
- Configurable timeouts
- Robust error handling

Example:
    >>> with SerialComm(port='COM3', baudrate=115200) as ser:
    ...     ser.write(b'AT\r\n')
    ...     response = ser.read_response()
"""

from __future__ import annotations

import time
import threading
from contextlib import contextmanager
from dataclasses import dataclass
from enum import Enum
from typing import Optional, Union, Callable, List

try:
    import serial
    from serial import Serial, SerialException, serialutil
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    Serial = object  # Type hint fallback


class Parity(Enum):
    """Serial parity options"""
    NONE = serial.PARITY_NONE if SERIAL_AVAILABLE else 'N'
    EVEN = serial.PARITY_EVEN if SERIAL_AVAILABLE else 'E'
    ODD = serial.PARITY_ODD if SERIAL_AVAILABLE else 'O'
    MARK = serial.PARITY_MARK if SERIAL_AVAILABLE else 'M'
    SPACE = serial.PARITY_SPACE if SERIAL_AVAILABLE else 'S'


class StopBits(Enum):
    """Serial stop bits options"""
    ONE = serial.STOPBITS_ONE if SERIAL_AVAILABLE else 1
    ONE_POINT_FIVE = serial.STOPBITS_ONE_POINT_FIVE if SERIAL_AVAILABLE else 1.5
    TWO = serial.STOPBITS_TWO if SERIAL_AVAILABLE else 2


@dataclass
class SerialConfig:
    """Serial port configuration.

    Attributes:
        port: Port name (e.g., 'COM3' on Windows, '/dev/ttyUSB0' on Linux)
        baudrate: Baud rate (common: 9600, 115200 for EC200A)
        parity: Parity checking
        stopbits: Number of stop bits
        bytesize: Number of data bits
        timeout: Read timeout in seconds
        write_timeout: Write timeout in seconds
        inter_byte_timeout: Inter-byte timeout in seconds
    """
    port: str
    baudrate: int = 115200
    parity: Union[Parity, str] = Parity.NONE
    stopbits: Union[StopBits, float] = StopBits.ONE
    bytesize: int = 8
    timeout: float = 1.0
    write_timeout: float = 1.0
    inter_byte_timeout: Optional[float] = None

    def to_serial_args(self) -> dict:
        """Convert to pyserial arguments.

        Returns:
            Dictionary of serial.Serial() arguments
        """
        return {
            'port': self.port,
            'baudrate': self.baudrate,
            'parity': self.parity.value if isinstance(self.parity, Parity) else self.parity,
            'stopbits': self.stopbits.value if isinstance(self.stopbits, StopBits) else self.stopbits,
            'bytesize': self.bytesize,
            'timeout': self.timeout,
            'write_timeout': self.write_timeout,
            'inter_byte_timeout': self.inter_byte_timeout,
        }


@dataclass
class ConnectionStats:
    """Connection statistics.

    Attributes:
        bytes_sent: Total bytes sent
        bytes_received: Total bytes received
        errors: Total error count
        reconnects: Total reconnection count
        connected_at: Timestamp of connection
        last_activity: Timestamp of last activity
    """
    bytes_sent: int = 0
    bytes_received: int = 0
    errors: int = 0
    reconnects: int = 0
    connected_at: float = 0
    last_activity: float = 0


class SerialComm:
    """Serial communication handler for EC200A modem.

    Provides thread-safe serial communication with automatic reconnection
    and comprehensive error handling.

    Example:
        >>> config = SerialConfig(port='COM3', baudrate=115200)
        >>> comm = SerialComm(config)
        >>> comm.connect()
        >>> comm.write(b'AT\r\n')
        >>> response = comm.read_line()
        >>> comm.close()
    """

    # EC200A default baud rates
    BAUDRATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]

    def __init__(
        self,
        config: Optional[SerialConfig] = None,
        port: str = '',
        baudrate: int = 115200,
        auto_reconnect: bool = True,
        reconnect_delay: float = 1.0,
        max_reconnect_attempts: int = 5
    ):
        """Initialize serial communicator.

        Args:
            config: SerialConfig object (overrides port/baudrate if provided)
            port: Serial port name
            baudrate: Baud rate
            auto_reconnect: Enable automatic reconnection on error
            reconnect_delay: Delay between reconnect attempts
            max_reconnect_attempts: Maximum reconnect attempts (0 = infinite)
        """
        if not SERIAL_AVAILABLE:
            raise ImportError("pyserial is required. Install with: pip install pyserial")

        # Resolve configuration
        if config is not None:
            self._config = config
        else:
            self._config = SerialConfig(port=port, baudrate=baudrate)

        self._connection: Optional[Serial] = None
        self._lock = threading.RLock()
        self._stats = ConnectionStats()

        # Reconnection settings
        self._auto_reconnect = auto_reconnect
        self._reconnect_delay = reconnect_delay
        self._max_reconnect_attempts = max_reconnect_attempts
        self._reconnect_count = 0

        # Event callbacks
        self._on_connect: Optional[Callable[[], None]] = None
        self._on_disconnect: Optional[Callable[[], None]] = None
        self._on_error: Optional[Callable[[Exception], None]] = None
        self._on_data: Optional[Callable[[bytes], None]] = None

    @property
    def is_connected(self) -> bool:
        """Check if connection is active.

        Returns:
            True if connected
        """
        with self._lock:
            return self._connection is not None and self._connection.is_open

    @property
    def serial_config(self) -> SerialConfig:
        """Get serial configuration.

        Returns:
            SerialConfig object
        """
        return self._config

    @property
    def stats(self) -> ConnectionStats:
        """Get connection statistics.

        Returns:
            Copy of current statistics
        """
        with self._lock:
            return ConnectionStats(**self._stats.__dict__)

    def connect(self) -> bool:
        """Establish serial connection.

        Returns:
            True if connection successful

        Raises:
            SerialException: If connection fails and auto-reconnect is disabled
        """
        with self._lock:
            if self.is_connected:
                return True

            args = self._config.to_serial_args()

            try:
                self._connection = Serial(**args)
                self._stats.connected_at = time.time()
                self._stats.last_activity = time.time()
                self._reconnect_count = 0

                if self._on_connect:
                    self._on_connect()

                return True

            except SerialException as e:
                self._stats.errors += 1
                if self._on_error:
                    self._on_error(e)

                if self._auto_reconnect:
                    return self._reconnect()
                raise

    def disconnect(self) -> None:
        """Close serial connection."""
        with self._lock:
            if self._connection is not None:
                try:
                    if self._connection.is_open:
                        self._connection.close()
                except SerialException:
                    pass
                finally:
                    self._connection = None

                if self._on_disconnect:
                    self._on_disconnect()

    def reconnect(self) -> bool:
        """Attempt to reconnect to the serial port.

        Returns:
            True if reconnection successful
        """
        with self._lock:
            self.disconnect()
            time.sleep(self._reconnect_delay)

            self._reconnect_count += 1
            if (self._max_reconnect_attempts > 0 and
                self._reconnect_count > self._max_reconnect_attempts):
                return False

            return self.connect()

    def _reconnect(self) -> bool:
        """Internal reconnection logic.

        Returns:
            True if successful
        """
        if self._auto_reconnect:
            return self.reconnect()
        return False

    def write(self, data: Union[str, bytes]) -> int:
        """Write data to serial port.

        Args:
            data: String or bytes to write

        Returns:
            Number of bytes written

        Raises:
            SerialException: If write fails
            ValueError: If not connected
        """
        if isinstance(data, str):
            data = data.encode('utf-8')

        with self._lock:
            if not self.is_connected:
                if not self._reconnect():
                    raise ValueError("Not connected")

            try:
                count = self._connection.write(data)  # type: ignore
                self._connection.flush()
                self._stats.bytes_sent += count
                self._stats.last_activity = time.time()
                return count

            except SerialException as e:
                self._stats.errors += 1
                if self._on_error:
                    self._on_error(e)

                if self._auto_reconnect and self._reconnect():
                    return self.write(data)
                raise

    def read(self, size: int = 1, timeout: Optional[float] = None) -> bytes:
        """Read bytes from serial port.

        Args:
            size: Maximum number of bytes to read
            timeout: Read timeout (overrides default if specified)

        Returns:
            Bytes read (may be empty if timeout)

        Raises:
            SerialException: If read fails
            ValueError: If not connected
        """
        with self._lock:
            if not self.is_connected:
                if not self._reconnect():
                    raise ValueError("Not connected")

            try:
                if timeout is not None:
                    old_timeout = self._connection.timeout
                    self._connection.timeout = timeout
                    data = self._connection.read(size)
                    self._connection.timeout = old_timeout
                else:
                    data = self._connection.read(size)

                if data:
                    self._stats.bytes_received += len(data)
                    self._stats.last_activity = time.time()

                    if self._on_data:
                        self._on_data(data)

                return data

            except SerialException as e:
                self._stats.errors += 1
                if self._on_error:
                    self._on_error(e)

                if self._auto_reconnect and self._reconnect():
                    return self.read(size, timeout)
                raise

    def read_line(self, terminator: bytes = b'\r\n', timeout: Optional[float] = None) -> bytes:
        """Read a line terminated by specific sequence.

        Args:
            terminator: Line terminator bytes
            timeout: Read timeout

        Returns:
            Line bytes (without terminator)

        Raises:
            SerialException: If read fails
        """
        line = bytearray()
        start_time = time.time()

        while True:
            # Check timeout
            if timeout is not None:
                elapsed = time.time() - start_time
                if elapsed > timeout:
                    break

            byte = self.read(1, timeout=max(0.1, timeout - elapsed if timeout else None))
            if not byte:
                break

            line.extend(byte)

            # Check for terminator
            if len(line) >= len(terminator):
                if line[-len(terminator):] == list(terminator):
                    return bytes(line[:-len(terminator)])

        return bytes(line)

    def read_all(self, timeout: Optional[float] = None) -> bytes:
        """Read all available bytes.

        Args:
            timeout: Time to wait for more data

        Returns:
            All available bytes
        """
        data = bytearray()
        start_time = time.time()

        while True:
            if timeout is not None:
                elapsed = time.time() - start_time
                if elapsed > timeout:
                    break

            chunk = self.read(1024, timeout=0.1)
            if not chunk:
                break

            data.extend(chunk)

        return bytes(data)

    def read_response(self, timeout: float = 1.0) -> bytes:
        """Read AT command response (until OK/ERROR or timeout).

        Args:
            timeout: Maximum time to wait for response

        Returns:
            Response bytes
        """
        response = bytearray()
        start_time = time.time()

        while time.time() - start_time < timeout:
            byte = self.read(1, timeout=0.1)
            if not byte:
                continue

            response.extend(byte)

            # Check for termination indicators
            if len(response) >= 4:
                if response[-4:] == b'OK\r\n' or response[-4:] == b'OK\n':
                    return bytes(response)
                if response[-5:] == b'ERROR\r\n' or response[-5:] == b'ERROR\n':
                    return bytes(response)
                if response[-7:] == b'OK\r\n\r\n' or response[-6:] == b'OK\n\n':
                    return bytes(response)

        return bytes(response)

    def flush(self) -> None:
        """Flush input and output buffers."""
        with self._lock:
            if self.is_connected:
                try:
                    self._connection.reset_input_buffer()
                    self._connection.reset_output_buffer()
                except SerialException:
                    pass

    def reset_stats(self) -> None:
        """Reset connection statistics."""
        with self._lock:
            self._stats = ConnectionStats()

    # Event handlers
    def on_connect(self, callback: Callable[[], None]) -> None:
        """Set connection event handler.

        Args:
            callback: Function to call when connected
        """
        self._on_connect = callback

    def on_disconnect(self, callback: Callable[[], None]) -> None:
        """Set disconnection event handler.

        Args:
            callback: Function to call when disconnected
        """
        self._on_disconnect = callback

    def on_error(self, callback: Callable[[Exception], None]) -> None:
        """Set error event handler.

        Args:
            callback: Function to call on error
        """
        self._on_error = callback

    def on_data(self, callback: Callable[[bytes], None]) -> None:
        """Set data received event handler.

        Args:
            callback: Function to call when data received
        """
        self._on_data = callback

    def __enter__(self):
        """Context manager entry."""
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.disconnect()

    def __repr__(self) -> str:
        return (f"SerialComm(port={self._config.port}, "
                f"baudrate={self._config.baudrate}, "
                f"connected={self.is_connected})")


# =============================================================================
# Utilities
# =============================================================================

def list_ports() -> List[str]:
    """List available serial ports.

    Returns:
        List of port names

    Example:
        >>> ports = list_ports()
        >>> for port in ports:
        ...     print(f"Available: {port}")
    """
    if not SERIAL_AVAILABLE:
        return []

    try:
        from serial.tools.list_ports import comports
        return [port.device for port in comports()]
    except ImportError:
        return []


def find_ec200a_port() -> Optional[str]:
    """Attempt to find EC200A serial port.

    Searches for common port names and USB IDs associated with
    Quectel EC200A module.

    Returns:
        Port name if found, None otherwise
    """
    if not SERIAL_AVAILABLE:
        return None

    try:
        from serial.tools.list_ports import comports

        # EC200A USB IDs (Quectel)
        quectel_vids = [0x2C7C]  # Quectel Vendor ID
        ec200a_pids = [0x0125, 0x6000]  # EC200A Product IDs

        for port in comports():
            # Check for known USB IDs
            vid = getattr(port, 'vid', None)
            pid = getattr(port, 'pid', None)

            if vid in quectel_pids and pid in ec200a_pids:
                return port.device

            # Check for Quectel in description
            description = getattr(port, 'description', '').lower()
            if 'quectel' in description or 'ec200' in description:
                return port.device

    except ImportError:
        pass

    return None


# =============================================================================
# Convenience Functions
# =============================================================================

def quick_connect(port: str = '', baudrate: int = 115200) -> SerialComm:
    """Quick connection convenience function.

    Args:
        port: Serial port (auto-detect if empty)
        baudrate: Baud rate

    Returns:
        Connected SerialComm instance

    Example:
        >>> comm = quick_connect()  # Auto-detect port
        >>> comm.write(b'AT\r\n')
        >>> print(comm.read_response())
        >>> comm.close()
    """
    if not port:
        port = find_ec200a_port() or ''

    if not port:
        raise ValueError("No serial port specified or detected")

    comm = SerialComm(port=port, baudrate=baudrate)
    comm.connect()
    return comm
