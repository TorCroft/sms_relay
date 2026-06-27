"""AT Command Module for EC200A

High-level AT command interface for Quectel EC200A cellular modem.
Provides structured command execution, response parsing, and error handling.

Features:
- Command queuing and execution
- Response parsing and validation
- Standard AT command helpers
- SMS-specific commands
- Network and connection commands

Example:
    >>> at = AtCommandHandler(serial_comm)
    >>> at.execute('AT')  # Basic connection test
    >>> result = at.execute('AT+CSQ')  # Signal quality
    >>> print(f'Signal: {result.rssi}')
"""

from __future__ import annotations

import re
import time
import threading
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, Union, List, Dict, Any, Callable, Tuple

from .serial import SerialComm


class AtResult(Enum):
    """AT command result codes"""
    OK = "OK"
    ERROR = "ERROR"
    CMS_ERROR = "+CMS ERROR"
    CME_ERROR = "+CME ERROR"
    NO_CARRIER = "NO CARRIER"
    NO_RESPONSE = "NO RESPONSE"
    TIMEOUT = "TIMEOUT"


@dataclass
class AtResponse:
    """Parsed AT command response.

    Attributes:
        success: True if command succeeded
        result_code: Final result code (OK, ERROR, etc.)
        raw: Raw response bytes
        lines: Response lines (without result code)
        data: Additional data from response (if any)
        error_code: Error code number (if error)
    """
    success: bool
    result_code: str
    raw: bytes
    lines: List[str] = field(default_factory=list)
    data: Dict[str, Any] = field(default_factory=dict)
    error_code: Optional[int] = None

    def __repr__(self) -> str:
        return f"AtResponse(success={self.success}, result={self.result_code})"


@dataclass
class NetworkInfo:
    """Network registration and signal information.

    Attributes:
        registered: True if registered to network
        mode: Registration mode (0=no, 1=home, 2=roaming, etc.)
        rssi: Received Signal Strength Indication (0-31)
        ber: Bit Error Rate (0-7)
        operator: Mobile operator name
        lac: Location Area Code
        ci: Cell ID
    """
    registered: bool = False
    mode: int = 0
    rssi: int = 99
    ber: int = 99
    operator: str = ""
    lac: str = ""
    ci: str = ""


@dataclass
class SmsStatus:
    """SMS message status.

    Attributes:
        used: Used memory slots
        total: Total memory slots
        max_pdu: Maximum PDU length supported
    """
    used: int = 0
    total: int = 0
    max_pdu: int = 160


class AtCommandHandler:
    """AT command handler for EC200A modem.

    Provides high-level AT command execution with automatic
    response parsing and error handling.

    Example:
        >>> comm = SerialComm(port='COM3', baudrate=115200)
        >>> comm.connect()
        >>> at = AtCommandHandler(comm)
        >>> response = at.execute('AT')
        >>> if response.success:
        ...     print("Modem ready")
    """

    # Standard timeouts (seconds)
    TIMEOUT_DEFAULT = 1.0
    TIMEOUT_SMS = 5.0
    TIMEOUT_NETWORK = 10.0

    def __init__(
        self,
        serial_comm: SerialComm,
        command_timeout: float = TIMEOUT_DEFAULT,
        echo: bool = True
    ):
        """Initialize AT command handler.

        Args:
            serial_comm: SerialComm instance
            command_timeout: Default command timeout
            echo: Enable command echo (ATE1/ATE0)
        """
        self._comm = serial_comm
        self._timeout = command_timeout
        self._echo = echo
        self._lock = threading.RLock()

        # Command state
        self._last_command: Optional[str] = None
        self._last_response: Optional[AtResponse] = None

    @property
    def is_ready(self) -> bool:
        """Check if modem is ready.

        Returns:
            True if serial connection is active
        """
        return self._comm.is_connected

    def execute(
        self,
        command: str,
        timeout: Optional[float] = None,
        expect_pattern: Optional[str] = None,
        check_result: bool = True
    ) -> AtResponse:
        """Execute AT command and parse response.

        Args:
            command: AT command string (without \r\n)
            timeout: Command-specific timeout
            expect_pattern: Regex pattern to expect in response
            check_result: Check for OK/ERROR in response

        Returns:
            AtResponse object

        Example:
            >>> response = at.execute('AT+CSQ')
            >>> if response.success:
            ...     print(f'RSSI: {response.data.get("rssi")}')
        """
        if timeout is None:
            timeout = self._timeout

        with self._lock:
            self._last_command = command

            # Send command (AT commands use ASCII encoding)
            cmd_bytes = (command + '\r\n').encode('ascii')
            self._comm.write(cmd_bytes)

            # Read response
            raw = self._comm.read_response(timeout=timeout)
            response = self._parse_response(raw, command)

            # Check for expected pattern
            if expect_pattern and response.success:
                for line in response.lines:
                    if re.search(expect_pattern, line):
                        break
                else:
                    response.success = False
                    response.result_code = "PATTERN_NOT_FOUND"

            self._last_response = response
            return response

    def execute_multi(
        self,
        commands: List[str],
        stop_on_error: bool = True,
        delay: float = 0.1
    ) -> List[AtResponse]:
        """Execute multiple commands in sequence.

        Args:
            commands: List of AT commands
            stop_on_error: Stop executing if command fails
            delay: Delay between commands

        Returns:
            List of AtResponse objects
        """
        responses = []

        for cmd in commands:
            response = self.execute(cmd)
            responses.append(response)

            if not response.success and stop_on_error:
                break

            time.sleep(delay)

        return responses

    def _parse_response(self, raw: bytes, command: str) -> AtResponse:
        """Parse raw AT response into AtResponse.

        Args:
            raw: Raw response bytes
            command: Original command string

        Returns:
            Parsed AtResponse
        """
        try:
            text = raw.decode('utf-8', errors='replace')
        except UnicodeDecodeError:
            text = raw.decode('latin-1', errors='replace')

        lines = [line.strip() for line in text.split('\n') if line.strip()]
        result_code = AtResult.TIMEOUT.value
        error_code = None

        # Remove echo line
        if lines and lines[0].startswith(command):
            lines = lines[1:]

        # Extract result code
        if lines:
            last_line = lines[-1].upper()

            if last_line == 'OK':
                result_code = AtResult.OK.value
            elif last_line == 'ERROR':
                result_code = AtResult.ERROR.value
            elif last_line.startswith('+CMS ERROR:'):
                result_code = AtResult.CMS_ERROR.value
                try:
                    error_code = int(last_line.split(':')[1].strip())
                except (ValueError, IndexError):
                    pass
            elif last_line.startswith('+CME ERROR:'):
                result_code = AtResult.CME_ERROR.value
                try:
                    error_code = int(last_line.split(':')[1].strip())
                except (ValueError, IndexError):
                    pass
            elif last_line == 'NO CARRIER':
                result_code = AtResult.NO_CARRIER.value
            else:
                result_code = AtResult.NO_RESPONSE.value

        # Remove result code from data lines
        if lines and any(rc.value == lines[-1].upper() for rc in AtResult):
            lines = lines[:-1]

        return AtResponse(
            success=(result_code == AtResult.OK.value),
            result_code=result_code,
            raw=raw,
            lines=lines,
            error_code=error_code
        )

    # =============================================================================
    # Standard AT Commands
    # =============================================================================

    def test(self) -> bool:
        """Test modem connection (AT).

        Returns:
            True if modem responds
        """
        response = self.execute('AT')
        return response.success

    def get_imei(self) -> Optional[str]:
        """Get device IMEI (AT+GSN).

        Returns:
            IMEI string or None
        """
        response = self.execute('AT+GSN')
        if response.success and response.lines:
            return response.lines[0].replace('\r', '').strip()
        return None

    def get_model(self) -> Optional[str]:
        """Get modem model identification (AT+GMM).

        Returns:
            Model string or None
        """
        response = self.execute('AT+GMM')
        if response.success and response.lines:
            return response.lines[0]
        return None

    def get_revision(self) -> Optional[str]:
        """Get firmware revision (AT+GMR).

        Returns:
            Revision string or None
        """
        response = self.execute('AT+GMR')
        if response.success and response.lines:
            return response.lines[0]
        return None

    def get_manufacturer(self) -> Optional[str]:
        """Get manufacturer identification (AT+GMI).

        Returns:
            Manufacturer string or None
        """
        response = self.execute('AT+GMI')
        if response.success and response.lines:
            return response.lines[0]
        return None

    def set_echo(self, enable: bool) -> bool:
        """Set command echo (ATE0/ATE1).

        Args:
            enable: True to enable echo, False to disable

        Returns:
            True if successful
        """
        cmd = 'ATE1' if enable else 'ATE0'
        response = self.execute(cmd)
        self._echo = enable
        return response.success

    # =============================================================================
    # Network Commands
    # =============================================================================

    def get_signal_quality(self) -> Optional[Dict[str, int]]:
        """Get signal quality (AT+CSQ).

        Returns:
            Dict with 'rssi' (0-31) and 'ber' (0-7), or None
            RSSI: 0=less than -113dBm, 31=great than -51dBm
            BER: 0=below 0.2%, 7=above 12.8%
        """
        response = self.execute('AT+CSQ')

        if response.success and response.lines:
            match = re.search(r'\+CSQ:\s*(\d+),\s*(\d+)', response.lines[0])
            if match:
                return {
                    'rssi': int(match.group(1)),
                    'ber': int(match.group(2))
                }
        return None

    def get_network_info(self) -> NetworkInfo:
        """Get network registration info (AT+CREG?, AT+COPS?).

        Returns:
            NetworkInfo object
        """
        info = NetworkInfo()

        # Get registration status
        response = self.execute('AT+CREG?')
        if response.success and response.lines:
            match = re.search(r'\+CREG:\s*(\d+),\s*([0-9A-Fa-f]+),\s*([0-9A-Fa-f]+)',
                               response.lines[0])
            if match:
                info.mode = int(match.group(1))
                info.lac = match.group(2)
                info.ci = match.group(3)
                info.registered = info.mode in (1, 5)  # Home or roaming

        # Get operator
        response = self.execute('AT+COPS?')
        if response.success and response.lines:
            match = re.search(r'\+COPS:\s*(\d+),\s*\d+,\s*"([^"]*)"', response.lines[0])
            if match:
                info.operator = match.group(2)

        # Get signal quality
        sig = self.get_signal_quality()
        if sig:
            info.rssi = sig['rssi']
            info.ber = sig['ber']

        return info

    def set_operator(self, operator: Optional[str] = None, mode: int = 0) -> bool:
        """Set network operator selection (AT+COPS).

        Args:
            operator: Operator name (None for auto)
            mode: 0=auto, 1=manual

        Returns:
            True if successful
        """
        if operator is None:
            cmd = f'AT+COPS={mode}'
        else:
            cmd = f'AT+COPS={mode},0,"{operator}"'

        response = self.execute(cmd, timeout=self.TIMEOUT_NETWORK)
        return response.success

    # =============================================================================
    # SMS Commands
    # =============================================================================

    def set_sms_format(self, pdu: bool = True) -> bool:
        """Set SMS format (AT+CMGF).

        Args:
            pdu: True for PDU mode, False for text mode

        Returns:
            True if successful
        """
        mode = 0 if pdu else 1
        response = self.execute(f'AT+CMGF={mode}')
        return response.success

    def get_sms_status(self) -> Optional[SmsStatus]:
        """Get SMS memory status (AT+CPMS?).

        Returns:
            SmsStatus object or None
        """
        response = self.execute('AT+CPMS?')
        if response.success and response.lines:
            match = re.search(r'\+CPMS:\s*"([^"]*)",\s*(\d+),\s*(\d+)', response.lines[0])
            if match:
                return SmsStatus(
                    used=int(match.group(2)),
                    total=int(match.group(3))
                )
        return None

    def list_sms(self, stat: int = 4) -> List[Dict[str, Any]]:
        """List SMS messages (AT+CMGL).

        Args:
            stat: Message status (0=REC UNREAD, 1=REC READ, 2=STORED UNSENT,
                  3=STORED SENT, 4=ALL)

        Returns:
            List of message dicts with index, status, pdu, length
        """
        response = self.execute(f'AT+CMGL={stat}', timeout=self.TIMEOUT_SMS)
        messages = []

        if response.success:
            # Parse CMGL responses
            # Format: +CMGL: <index>,<stat>,[<alpha>],<length><CR><LF><pdu>
            lines = response.lines
            i = 0
            while i < len(lines):
                line = lines[i]
                match = re.match(r'\+CMGL:\s*(\d+),\s*(\d+),,?\s*(\d+)', line)
                if match:
                    # PDU is on the next line
                    pdu = ""
                    if i + 1 < len(lines):
                        pdu = lines[i + 1].strip()

                    messages.append({
                        'index': int(match.group(1)),
                        'status': int(match.group(2)),
                        'length': int(match.group(3)),
                        'pdu': pdu
                    })
                    i += 2  # Skip PDU line
                else:
                    i += 1

        return messages

    def read_sms(self, index: int) -> Optional[str]:
        """Read SMS message at index (AT+CMGR).

        Args:
            index: Message index

        Returns:
            PDU string or None
        """
        response = self.execute(f'AT+CMGR={index}', timeout=self.TIMEOUT_SMS)
        if response.success and len(response.lines) > 1:
            # Return PDU (second line onwards)
            return response.lines[1]
        return None

    def delete_sms(self, index: int) -> bool:
        """Delete SMS message at index (AT+CMGD).

        Args:
            index: Message index

        Returns:
            True if successful
        """
        response = self.execute(f'AT+CMGD={index}')
        return response.success

    def delete_all_sms(self) -> bool:
        """Delete all SMS messages (AT+CMGD=1,4).

        Returns:
            True if successful
        """
        response = self.execute('AT+CMGD=1,4')
        return response.success

    def send_sms(self, pdu: str, length: int) -> bool:
        """Send SMS message (AT+CMGS).

        Args:
            pdu: PDU hex string
            length: TPDU length

        Returns:
            True if accepted for sending
        """
        # Start send
        self._comm.write(f'AT+CMGS={length}\r\n'.encode('ascii'))

        # Wait for prompt
        prompt = self._comm.read(1, timeout=2.0)
        if prompt != b'>':
            return False

        # Send PDU with Ctrl+Z terminator
        self._comm.write(pdu.encode('ascii'))
        self._comm.write(bytes([26]))  # Ctrl+Z

        # Read response
        response = self._comm.read_response(timeout=self.TIMEOUT_SMS)
        parsed = self._parse_response(response, 'AT+CMGS')

        return parsed.success

    def set_sms_notification(
        self,
        mode: int = 2,
        mt: int = 1,
        bm: int = 0,
        ds: int = 1,
        bfr: int = 0
    ) -> bool:
        """Set new SMS notification (AT+CNMI).

        Configures how the modem handles incoming SMS messages.
        When a new SMS arrives, the modem can automatically notify the host.

        Args:
            mode: Result code indication mode (TE处于活动状态时)
                0 = 缓冲TA中的非请求结果码
                1 = 链路保留时丢弃，否则直接转发给TE
                2 = 链路保留时缓冲并转发，否则直接转发 [推荐]
                3 = 类似mode=0，但无论链路状态都转发给TE
            mt: SMS-DELIVER (新短信) 处理方式
                0 = 不通知TE
                1 = 存储至ME/TA，发送 +CMTI: <mem>,<index> 通知 [推荐]
                2 = 直接发送PDU: +CMT: <length><CR><LF><pdu> (第2类除外)
                3 = 第3类短信用+ CMT，其他用+CMTI
            bm: CBM (小区广播) 处理方式
                0 = 不通知TE
                2 = 直接发送: +CBM: <length><CR><LF><pdu>
            ds: SMS-STATUS-REPORT (状态报告) 处理方式
                0 = 不通知TE
                1 = 直接发送: +CDS: <length><CR><LF><pdu> [推荐]
                2 = 存储后发送: +CDSI: <mem>,<index>
            bfr: TA缓冲区处理
                0 = 清空缓冲区中的非请求结果码
                1 = 保留缓冲区中的非请求结果码

        Returns:
            True if successful

        Example:
            >>> # 推荐配置：mode=2, mt=1, bm=0, ds=1, bfr=0
            >>> # 优点：有备份，通过+CMTI通知，可随时读取
            >>> at.set_sms_notification(mode=2, mt=1, bm=0, ds=1, bfr=0)
            True
            >>>
            >>> # 快速配置：mode=2, mt=2
            >>> # 优点：直接收到PDU，无存储延迟
            >>> # 缺点：无备份，处理失败则丢失
            >>> at.set_sms_notification(mode=2, mt=2, bm=0, ds=1, bfr=0)
            True

        Note:
            推荐使用 mode=2, mt=1 组合：
            - 短信会先存储到SIM卡/模块内存（有备份）
            - 通过 +CMTI: "SM",<index> 立即通知
            - 可以在合适的时间读取PDU
            - 即使处理失败，短信仍在内存中

            如果需要最快响应速度，使用 mt=2：
            - 短信到达时直接通过 +CMT 发送PDU
            - 无需再读取内存
            - 但处理失败会导致短信丢失
        """
        cmd = f'AT+CNMI={mode},{mt},{bm},{ds},{bfr}'
        response = self.execute(cmd)
        return response.success

    def get_sms_notification(self) -> Optional[Dict[str, int]]:
        """Get current SMS notification settings (AT+CNMI?).

        Returns:
            Dict with current settings or None
        """
        response = self.execute('AT+CNMI?')
        if response.success and response.lines:
            match = re.search(r'\+CNMI:\s*(\d+),(\d+),(\d+),(\d+),(\d+)', response.lines[0])
            if match:
                return {
                    'mode': int(match.group(1)),
                    'mt': int(match.group(2)),
                    'bm': int(match.group(3)),
                    'ds': int(match.group(4)),
                    'bfr': int(match.group(5))
                }
        return None

    def read_notification(self) -> Optional[str]:
        """Read incoming SMS notification (blocking).

        Waits for +CMTI or +CMT indication from modem.

        Returns:
            Notification line or None

        Example:
            >>> # Set up auto-notification first
            >>> at.set_sms_notification(mode=3, mt=1)
            >>>
            >>> # Wait for new SMS
            >>> notification = at.read_notification()
            >>> if notification:
            ...     print(f"New SMS: {notification}")
        """
        # Wait for notification (timeout = 2x default)
        raw = self._comm.read_response(timeout=self._timeout * 2)
        text = raw.decode('ascii', errors='replace').strip()

        if text.startswith('+CMTI') or text.startswith('+CMT'):
            return text
        return None

    def disable_sms_notification(self) -> bool:
        """Disable SMS auto-notification (AT+CNMI=0,0,0,0,0).

        Returns:
            True if successful

        Note:
            This disables automatic notifications. You'll need to poll
            for new messages using list_sms().
        """
        return self.set_sms_notification(mode=0, mt=0, bm=0, ds=0, bfr=0)

    # =============================================================================
    # SIM Commands
    # =============================================================================

    def get_sim_ccid(self) -> Optional[str]:
        """Get SIM card ICCID (AT+CCID).

        Returns:
            ICCID string or None
        """
        response = self.execute('AT+CCID')
        if response.success and response.lines:
            return response.lines[0]
        return None

    def get_sim_status(self) -> Optional[int]:
        """Get SIM card status (AT+CPIN?).

        Returns:
            Status code: 0=ready, 1=SIM PIN required, 2=PUK required
        """
        response = self.execute('AT+CPIN?')
        if response.success and response.lines:
            line = response.lines[0].upper()
            if 'READY' in line:
                return 0
            if '+CME ERROR' in line:
                return 1
        return None

    # =============================================================================
    # Module Control
    # =============================================================================

    def reset(self) -> bool:
        """Reset modem (AT+CRESET).

        Returns:
            True if command accepted
        """
        response = self.execute('AT+CRESET')
        return response.success

    def set_verbose(self, enable: bool) -> bool:
        """Set verbose error messages (AT+CMEE).

        Args:
            enable: True for detailed error codes

        Returns:
            True if successful
        """
        mode = 2 if enable else 0
        response = self.execute(f'AT+CMEE={mode}')
        return response.success

    def __repr__(self) -> str:
        return f"AtCommandHandler(ready={self.is_ready}, echo={self._echo})"


# =============================================================================
# Convenience Functions
# =============================================================================

def quick_at(port: str = '', baudrate: int = 115200) -> Tuple[SerialComm, AtCommandHandler]:
    """Quick AT interface setup.

    Args:
        port: Serial port (auto-detect if empty)
        baudrate: Baud rate

    Returns:
        Tuple of (SerialComm, AtCommandHandler)

    Example:
        >>> comm, at = quick_at()
        >>> if at.test():
        ...     print(f'Modem: {at.get_model()}')
        >>> comm.close()
    """
    from .serial import quick_connect
    comm = quick_connect(port=port, baudrate=baudrate)
    at = AtCommandHandler(comm)
    return comm, at
