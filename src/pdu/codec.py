"""GSM SMS PDU Codec

Modern Python 3 implementation of GSM 03.40 PDU encoding/decoding.
Supports SMS-SUBMIT, SMS-DELIVER, and SMS-STATUS-REPORT message types.

Reference: 3GPP TS 23.040 (GSM 03.40)
"""

from __future__ import annotations

import sys
import math
from datetime import datetime, timedelta, tzinfo
from copy import copy
from typing import Union, List, Tuple, Dict, Any, Optional, Iterator

# =============================================================================
# Constants
# =============================================================================

# GSM 7-bit default alphabet (GSM 03.38 section 6.2.1)
GSM7_BASIC = (
    "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?¡"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ`¿abcdefghijklmnopqrstuvwxyzäöñüà"
)

# GSM 7-bit extended table (GSM 03.38 section 6.2.2)
# Key: character, Value: escape sequence value
GSM7_EXTENDED: Dict[str, int] = {
    "\xff": 0x0A,  # Form feed
    "^": 0x14,      # Carriage return (wait, that's 0x0D)
    "{": 0x28,      # Left curly bracket
    "}": 0x29,      # Right curly bracket
    "\\": 0x2F,     # Backslash
    "[": 0x3C,      # Left square bracket
    "~": 0x3D,      # Tilde
    "]": 0x3E,      # Right square bracket
    "|": 0x40,      # Vertical bar
    "€": 0x65,      # Euro sign
}

# Maximum message length (in characters) per PDU for each encoding scheme
# GSM-7: 160 septets (180 bytes packed)
# 8-bit: 140 bytes
# UCS-2: 70 characters (140 bytes)
MAX_MESSAGE_LENGTH: Dict[int, int] = {
    0x00: 160,  # GSM-7 default alphabet
    0x04: 140,  # 8-bit data
    0x08: 70,   # UCS-2 (UTF-16 like)
}


# =============================================================================
# Timezone Handling
# =============================================================================

class SmsPduTzInfo(tzinfo):
    """Timezone info for SMS PDU timestamps.

    GSM 03.40 specifies timezone as a semi-octet encoded offset
    from UTC in quarter-hour increments.
    """

    def __init__(self, pdu_offset_str: Optional[str] = None):
        """Initialize timezone from PDU offset string.

        Args:
            pdu_offset_str: 2-digit semi-octet timezone offset (e.g., "68" for +UTC 2:30)
                           The MSB indicates sign (0x80 = negative).

        Note:
            pdu_offset_str is optional to support pickling, but should be
            set before using the instance for timezone calculations.
        """
        self._offset: Optional[timedelta] = None
        if pdu_offset_str is not None:
            self._set_from_pdu_string(pdu_offset_str)

    def _set_from_pdu_string(self, pdu_offset_str: str) -> None:
        """Parse and set offset from PDU semi-octet string."""
        tz_hex_val = int(pdu_offset_str, 16)

        if tz_hex_val & 0x80 == 0:
            # Positive offset: value * 15 minutes
            self._offset = timedelta(minutes=int(pdu_offset_str) * 15)
        else:
            # Negative offset: clear MSB, calculate, negate
            magnitude = int(f"{tz_hex_val & 0x7F:0>2X}") * -15
            self._offset = timedelta(minutes=magnitude)

    def utcoffset(self, dt: datetime) -> Optional[timedelta]:
        """Return UTC offset as timedelta."""
        return self._offset

    def dst(self, dt: datetime) -> timedelta:
        """Return DST offset (always 0 for GSM)."""
        return timedelta(0)


# =============================================================================
# User Data Header (UDH) Information Elements
# =============================================================================

class InformationElement:
    """Base class for UDH Information Elements (IE).

    Represents a single IE in the User Data Header. Subclasses handle
    specific IE types (Concatenation, PortAddress) automatically via
    the IEI (Information Element Identifier) registry.

    Attributes:
        id: IEI identifier byte
        data_length: Length of IE data in bytes
        data: Raw IE data bytes
    """

    # Instance variable type hints
    id: int
    data_length: int
    data: List[int]

    # IEI registry: maps IEI values to concrete classes
    _iei_class_map: Dict[int, type] = {}

    def __init_subclass__(cls, **kwargs):
        """Register subclasses with their IEI in the registry."""
        super().__init_subclass__(**kwargs)
        if hasattr(cls, "_iei_id"):
            cls._iei_class_map[cls._iei_id] = cls

    def __new__(cls, iei: int, ie_len: int = 0, ie_data: Optional[List[int]] = None):
        """Create appropriate IE subclass based on IEI."""
        target_class = cls._iei_class_map.get(iei, cls)
        if target_class is not cls:
            return object.__new__(target_class)
        return super().__new__(cls)

    def __init__(self, iei: int, ie_len: int = 0, ie_data: Optional[List[int]] = None):
        self.id = iei
        self.data_length = ie_len
        self.data = ie_data or []

    @classmethod
    def decode(cls, byte_iter: Iterator[int]) -> InformationElement:
        """Decode IE from byte iterator.

        Args:
            byte_iter: Iterator over PDU bytes

        Returns:
            InformationElement instance (or subclass)
        """
        iei = next(byte_iter)
        ie_len = next(byte_iter)
        ie_data = [next(byte_iter) for _ in range(ie_len)]
        return InformationElement(iei, ie_len, ie_data)

    def encode(self) -> bytearray:
        """Encode IE to bytes."""
        result = bytearray()
        result.append(self.id)
        result.append(self.data_length)
        result.extend(self.data)
        return result

    def __len__(self) -> int:
        """Total IE length in bytes (IEI + length + data)."""
        return self.data_length + 2

    def __repr__(self) -> str:
        return f"InformationElement(id={self.id:#x}, data_length={self.data_length})"


class Concatenation(InformationElement):
    """Concatenated SMS (CSMS) Information Element.

    Indicates this message is part of a multi-part SMS. Supports both
    8-bit (IEI=0x00) and 16-bit (IEI=0x08) reference numbers.

    Attributes:
        reference: CSMS reference number (same for all parts)
        parts: Total number of parts in the concatenated message
        number: This part's sequence number (1-indexed)
    """

    _iei_id = 0x00  # 8-bit reference number IEI

    # Instance variable type hints
    reference: int
    parts: int
    number: int

    def __init__(self, iei: int = 0x00, ie_len: int = 0, ie_data: Optional[List[int]] = None):
        super().__init__(iei, ie_len, ie_data)
        if ie_data is not None:
            if iei == 0x00:  # 8-bit reference
                self.reference, self.parts, self.number = ie_data
            elif iei == 0x08:  # 16-bit reference
                self.reference = (ie_data[0] << 8) | ie_data[1]
                self.parts = ie_data[2]
                self.number = ie_data[3]
            else:
                # Invalid IEI for this type, use defaults
                self.reference = 0
                self.parts = 1
                self.number = 1

    def encode(self) -> bytearray:
        """Encode concatenation IE, auto-selecting 8 or 16-bit format."""
        if self.reference > 0xFF:
            # Use 16-bit reference format
            self.id = 0x08
            self.data = [
                self.reference >> 8, self.reference & 0xFF,
                self.parts, self.number
            ]
        else:
            # Use 8-bit reference format
            self.id = 0x00
            self.data = [self.reference, self.parts, self.number]
        self.data_length = len(self.data)
        return super().encode()

    def __repr__(self) -> str:
        return f"Concatenation(ref={self.reference}, part={self.number}/{self.parts})"


class PortAddress(InformationElement):
    """Application Port Addressing Scheme IE.

    Used for WAP push and other port-addressed applications.
    Supports both 8-bit (IEI=0x04) and 16-bit (IEI=0x05) port numbers.

    Attributes:
        destination: Destination port number
        source: Source port number
    """

    _iei_id = 0x04  # 8-bit port addressing IEI

    # Instance variable type hints
    destination: int
    source: int

    def __init__(self, iei: int = 0x04, ie_len: int = 0, ie_data: Optional[List[int]] = None):
        super().__init__(iei, ie_len, ie_data)
        if ie_data is not None:
            if iei == 0x04:  # 8-bit port addressing
                self.destination, self.source = ie_data
            elif iei == 0x05:  # 16-bit port addressing
                self.destination = (ie_data[0] << 8) | ie_data[1]
                self.source = (ie_data[2] << 8) | ie_data[3]
            else:
                self.destination = 0
                self.source = 0

    def encode(self) -> bytearray:
        """Encode port address IE, auto-selecting 8 or 16-bit format."""
        if self.destination > 0xFF or self.source > 0xFF:
            # Use 16-bit port format
            self.id = 0x05
            self.data = [
                self.destination >> 8, self.destination & 0xFF,
                self.source >> 8, self.source & 0xFF
            ]
        else:
            # Use 8-bit port format
            self.id = 0x04
            self.data = [self.destination, self.source]
        self.data_length = len(self.data)
        return super().encode()

    def __repr__(self) -> str:
        return f"PortAddress(dest={self.destination}, src={self.source})"


# Register additional IEI mappings
InformationElement._iei_class_map.update({
    0x08: Concatenation,  # 16-bit concatenation
    0x05: PortAddress,    # 16-bit port addressing
})


# =============================================================================
# PDU Data Structure
# =============================================================================

class Pdu:
    """Encoded SMS PDU container.

    Attributes:
        data: Raw PDU bytes
        tpdu_length: Length of TPDU portion (excluding SMC)
    """

    def __init__(self, data: bytearray, tpdu_length: int):
        self.data = data
        self.tpdu_length = tpdu_length

    def __str__(self) -> str:
        """Return PDU as uppercase hex string."""
        return self.data.hex().upper()

    def __repr__(self) -> str:
        return f"Pdu(tpdu_length={self.tpdu_length}, data={str(self)})"


# =============================================================================
# Public API - PDU Encoding/Decoding
# =============================================================================

def decode_pdu(pdu: Union[str, bytes, bytearray]) -> Dict[str, Any]:
    """Decode SMS PDU from hex string or bytes.

    Parses the PDU and extracts all fields including SMSC, addresses,
    timestamp, user data header, and message text.

    Args:
        pdu: PDU data as hex string, bytes, or bytearray

    Returns:
        Dictionary containing decoded PDU fields:
            - 'smsc': SMSC phone number
            - 'type': Message type ('SMS-DELIVER', 'SMS-SUBMIT', 'SMS-STATUS-REPORT')
            - 'number': Sender/recipient phone number
            - 'text': Decoded message text
            - 'udh': List of InformationElement objects (if present)
            - 'time', 'reference', 'validity', etc. (varies by type)

    Raises:
        ValueError: If PDU data is invalid or cannot be decoded

    Example:
        >>> result = decode_pdu("07919761980128F0040B91234143020...")

    """
    try:
        pdu_bytes = _as_bytearray(pdu)
    except Exception as e:
        raise ValueError(f"Cannot convert PDU to bytearray: {e}") from e

    result: Dict[str, Any] = {}
    pdu_iter = iter(pdu_bytes)

    # Parse SMSC address field
    smsc_number, smsc_bytes_read = _parse_address_field(pdu_iter, smsc_field=True)
    result["smsc"] = smsc_number
    result["tpdu_length"] = len(pdu_bytes) - smsc_bytes_read

    # Get TPDU first octet to determine message type
    tpdu_first_octet = next(pdu_iter)
    pdu_type = tpdu_first_octet & 0x03  # Bits 0-1: Message Type

    if pdu_type == 0x00:  # SMS-DELIVER or SMS-DELIVER-REPORT
        result["type"] = "SMS-DELIVER"
        result["number"] = _parse_address_field(pdu_iter)[0]
        result["protocol_id"] = next(pdu_iter)
        data_coding = _parse_data_coding_scheme(next(pdu_iter))
        result["time"] = _parse_timestamp(pdu_iter)
        user_data_len = next(pdu_iter)
        udh_present = (tpdu_first_octet & 0x40) != 0  # UDHI bit
        ud = _parse_user_data(pdu_iter, user_data_len, data_coding, udh_present)
        result.update(ud)

    elif pdu_type == 0x01:  # SMS-SUBMIT or SMS-SUBMIT-REPORT
        result["type"] = "SMS-SUBMIT"
        result["reference"] = next(pdu_iter)  # Message reference
        result["number"] = _parse_address_field(pdu_iter)[0]
        result["protocol_id"] = next(pdu_iter)
        data_coding = _parse_data_coding_scheme(next(pdu_iter))

        # Parse validity period if present
        validity_period_format = (tpdu_first_octet & 0x18) >> 3  # Bits 3-4
        if validity_period_format == 0x02:  # Relative format
            result["validity"] = _parse_validity_period(next(pdu_iter))
        elif validity_period_format == 0x03:  # Absolute format
            result["validity"] = _parse_timestamp(pdu_iter)

        user_data_len = next(pdu_iter)
        udh_present = (tpdu_first_octet & 0x40) != 0  # UDHI bit
        ud = _parse_user_data(pdu_iter, user_data_len, data_coding, udh_present)
        result.update(ud)

    elif pdu_type == 0x02:  # SMS-STATUS-REPORT or SMS-COMMAND
        result["type"] = "SMS-STATUS-REPORT"
        result["reference"] = next(pdu_iter)
        result["number"] = _parse_address_field(pdu_iter)[0]
        result["time"] = _parse_timestamp(pdu_iter)  # Discharge time
        result["discharge"] = _parse_timestamp(pdu_iter)  # Sent time
        result["status"] = next(pdu_iter)

    else:
        raise ValueError(f"Unknown message type: {pdu_type}")

    return result


def encode_submit_pdu(
    number: str,
    text: str,
    reference: int = 0,
    validity: Optional[Union[timedelta, datetime]] = None,
    smsc: Optional[str] = None,
    request_status_report: bool = True,
    reject_duplicates: bool = False,
    send_flash: bool = False
) -> List[Pdu]:
    """Build SMS-SUBMIT PDU(s) for sending a text message.

    Automatically handles message concatenation for long messages and
    selects the appropriate encoding (GSM-7 or UCS-2).

    Args:
        number: Destination phone number (international format: +1234567890)
        text: Message text to send
        reference: Message reference number (for duplicate detection)
        validity: Message validity period (timedelta for relative, datetime for absolute)
        smsc: SMSC phone number (None = use default)
        request_status_report: Request delivery status report
        reject_duplicates: Reject duplicates with same reference/destination
        send_flash: Send as Class 0 flash message

    Returns:
        List of Pdu objects (may be multiple for concatenated messages)

    Example:
        >>> pdus = encode_submit_pdu("+1234567890", "Hello, World!")
        >>> for pdu in pdus:
        ...     print(str(pdu))

    """
    # Build TPDU first octet
    tpdu_first_octet = 0x01  # SMS-SUBMIT message type

    # Set validity period format bits
    if validity is not None:
        if isinstance(validity, timedelta):
            tpdu_first_octet |= 0x10  # Relative validity
            validity_period = [_build_validity_period(validity)]
        elif isinstance(validity, datetime):
            tpdu_first_octet |= 0x18  # Absolute validity
            validity_period = _build_timestamp(validity)
        else:
            raise TypeError(
                "validity must be timedelta (relative) or datetime (absolute)"
            )
    else:
        validity_period = None

    # Set control bits
    if reject_duplicates:
        tpdu_first_octet |= 0x04  # TP-RD bit
    if request_status_report:
        tpdu_first_octet |= 0x20  # TP-SRR bit

    # Determine encoding and encode text
    try:
        encoded_text = gsm7_encode(text)
        alphabet = 0x00  # GSM-7
    except ValueError:
        alphabet = 0x08  # UCS-2
        encoded_text = ucs2_encode(text)

    # Handle concatenation for long messages
    max_len = MAX_MESSAGE_LENGTH[alphabet]
    if len(text) > max_len:
        concat_header_prototype = Concatenation()
        concat_header_prototype.reference = reference
        pdu_count = len(text) // max_len + 1
        concat_header_prototype.parts = pdu_count
        tpdu_first_octet |= 0x40  # Set UDHI bit
    else:
        concat_header_prototype = None
        pdu_count = 1

    # Build each PDU in the sequence
    pdus: List[Pdu] = []

    for i in range(pdu_count):
        pdu = bytearray()

        # Add SMSC (or 0x00 for default)
        if smsc:
            pdu.extend(_build_address_field(smsc, smsc_field=True))
        else:
            pdu.append(0x00)

        # Build UDH if concatenating
        udh = bytearray()
        if concat_header_prototype is not None:
            concat_header = copy(concat_header_prototype)
            concat_header.number = i + 1

            # Split text for this PDU
            if alphabet == 0x00:  # GSM-7: 153 chars per PDU with UDH
                pdu_text = text[i * 153:(i + 1) * 153]
            elif alphabet == 0x08:  # UCS-2: 67 chars per PDU with UDH
                pdu_text = text[i * 67:(i + 1) * 67]
            else:  # 8-bit: 140 bytes per PDU with UDH
                pdu_text = text[i * 140:(i + 1) * 140]

            udh.extend(concat_header.encode())
        else:
            pdu_text = text

        udh_len = len(udh)

        # Add TPDU header
        pdu.append(tpdu_first_octet)
        pdu.append(reference)  # Message reference (MR)
        pdu.extend(_build_address_field(number))  # Destination address
        pdu.append(0x00)  # Protocol identifier (no higher-layer protocol)

        # Add data coding scheme
        if send_flash:
            pdu.append(0x10 if alphabet == 0x00 else 0x18)  # Class 0 message
        else:
            pdu.append(alphabet)

        # Add validity period if present
        if validity_period:
            pdu.extend(validity_period)

        # Encode user data
        if alphabet == 0x00:  # GSM-7
            encoded_text = gsm7_encode(pdu_text)
            user_data_length = len(encoded_text)  # Length in septets
            if udh_len > 0:
                shift = ((udh_len + 1) * 8) % 7  # Fill bits for septet alignment
                user_data = septets_pack(encoded_text, pad_bits=shift)
                if shift > 0:
                    user_data_length += 1
            else:
                user_data = septets_pack(encoded_text)
        elif alphabet == 0x08:  # UCS-2
            user_data = ucs2_encode(pdu_text)
            user_data_length = len(user_data)
        else:  # 8-bit data
            user_data = bytearray(pdu_text, "latin-1")
            user_data_length = len(user_data)

        # Add user data length and UDH
        if udh_len > 0:
            user_data_length += udh_len + 1  # Include UDH length byte
            pdu.append(user_data_length)
            pdu.append(udh_len)
            pdu.extend(udh)
        else:
            pdu.append(user_data_length)

        pdu.extend(user_data)

        tpdu_length = len(pdu) - 1
        pdus.append(Pdu(pdu, tpdu_length))

    return pdus


# =============================================================================
# Internal Parsers and Builders
# =============================================================================

def _parse_user_data(
    byte_iter: Iterator[int],
    user_data_len: int,
    data_coding: int,
    udh_present: bool
) -> Dict[str, Any]:
    """Parse user data field (UDH + message text).

    Args:
        byte_iter: Iterator over PDU bytes
        user_data_len: User data length field value
        data_coding: Data coding scheme value
        udh_present: True if UDH is present

    Returns:
        Dictionary with 'text' and 'udh' keys
    """
    result: Dict[str, Any] = {}
    result["udh"] = []

    prev_octet: Optional[int] = None
    shift = 7

    # Parse UDH if present
    if udh_present:
        udh_len = next(byte_iter)
        ie_len_read = 0

        while ie_len_read < udh_len:
            ie = InformationElement.decode(byte_iter)
            ie_len_read += len(ie)
            result["udh"].append(ie)

        # Calculate fill bits for GSM-7 septet alignment
        if data_coding == 0x00:
            shift = ((udh_len + 1) * 8) % 7
            prev_octet = next(byte_iter)
            shift += 1

    # Decode message text based on coding scheme
    if data_coding == 0x00:  # GSM-7
        if udh_present:
            user_data_septets = septets_unpack(byte_iter, user_data_len, prev_octet, shift)
        else:
            user_data_septets = septets_unpack(byte_iter, user_data_len)
        result["text"] = gsm7_decode(user_data_septets)

    elif data_coding == 0x02:  # UCS-2
        result["text"] = ucs2_decode(byte_iter, user_data_len)

    else:  # 8-bit data
        user_data = [chr(b) for b in byte_iter]
        result["text"] = "".join(user_data)

    return result


def _parse_validity_period(tp_vp: int) -> timedelta:
    """Parse relative validity period from TP-VP value.

    TP-VP encoding follows GSM 03.40 section 9.2.3.12.

    Args:
        tp_vp: TP-VP field value (0-255)

    Returns:
        timedelta representing the validity period

    Raises:
        ValueError: If tp_vp is out of range
    """
    if tp_vp <= 143:
        # 0-143: (tp_vp + 1) * 5 minutes
        return timedelta(minutes=(tp_vp + 1) * 5)
    elif 144 <= tp_vp <= 167:
        # 144-167: 12 hours + (tp_vp - 143) * 30 minutes
        return timedelta(hours=12, minutes=(tp_vp - 143) * 30)
    elif 168 <= tp_vp <= 196:
        # 168-196: (tp_vp - 166) days
        return timedelta(days=(tp_vp - 166))
    elif 197 <= tp_vp <= 255:
        # 197-255: (tp_vp - 192) weeks
        return timedelta(weeks=(tp_vp - 192))
    else:
        raise ValueError("tp_vp must be in range [0, 255]")


def _build_validity_period(validity_period: timedelta) -> int:
    """Encode validity period timedelta to TP-VP value.

    TP-VP encoding follows GSM 03.40 section 9.2.3.12.

    Args:
        validity_period: Time delta to encode

    Returns:
        TP-VP field value (0-255)

    Raises:
        ValueError: If validity period is too long (> 441 days)
    """
    seconds = validity_period.seconds + (validity_period.days * 24 * 3600)

    if seconds <= 43200:  # <= 12 hours
        tp_vp = seconds // 300 - 1  # (minutes / 5) - 1
    elif seconds <= 86400:  # <= 24 hours
        tp_vp = (seconds - 43200) // 1800 + 143
    elif validity_period.days <= 30:  # <= 30 days
        tp_vp = validity_period.days + 166
    elif validity_period.days <= 441:  # <= 441 days (~63 weeks)
        tp_vp = validity_period.days // 7 + 192
    else:
        raise ValueError("Validity period too long (max 441 days)")

    return tp_vp


def _parse_timestamp(byte_iter: Iterator[int]) -> datetime:
    """Parse 7-octet GSM timestamp.

    Timestamp format: YYMMDDHHMMSS + timezone (semi-octet encoded)

    Args:
        byte_iter: Iterator over PDU bytes

    Returns:
        datetime with SmsPduTzInfo timezone
    """
    date_str = semi_octets_decode(byte_iter, 7)
    tz_str = date_str[-2:]
    dt = datetime.strptime(date_str[:-2], "%y%m%d%H%M%S")
    return dt.replace(tzinfo=SmsPduTzInfo(tz_str))


def _build_timestamp(timestamp: datetime) -> bytearray:
    """Encode datetime to 7-octet GSM timestamp.

    Args:
        timestamp: datetime with tzinfo set

    Returns:
        7-byte encoded timestamp

    Raises:
        ValueError: If timestamp lacks timezone info
    """
    if timestamp.tzinfo is None:
        raise ValueError("Timestamp must have timezone info")

    tz_delta = timestamp.utcoffset()
    if tz_delta is None:
        raise ValueError("Timestamp must have UTC offset")

    # Encode timezone offset
    if tz_delta.days >= 0:
        tz_str = f"{tz_delta.seconds // 60 // 15:0>2}"
    else:
        tz_val = (tz_delta.days * -3600 * 24 - tz_delta.seconds) // 60 // 15
        tz_val = int(f"{tz_val:0>2}", 16) | 0x80  # Set sign bit
        tz_str = f"{tz_val:0>2X}"

    date_str = timestamp.strftime("%y%m%d%H%M%S") + tz_str
    return semi_octets_encode(date_str)


def _parse_data_coding_scheme(octet: int) -> int:
    """Parse data coding scheme from DCS octet.

    Returns the alphabet encoding:
        0x00: GSM-7 default alphabet
        0x01: 8-bit data
        0x02: UCS-2

    Args:
        octet: DCS field value

    Returns:
        Alphabet encoding value
    """
    if octet & 0xC0 == 0:  # General data coding group
        alphabet = (octet & 0x0C) >> 2
        return alphabet
    return 0  # Default to GSM-7 for other groups


def _parse_address_field(
    byte_iter: Iterator[int],
    smsc_field: bool = False
) -> Tuple[Optional[str], int]:
    """Parse GSM address field.

    Supports both numeric (semi-octet) and alphanumeric (GSM-7) addresses.

    Args:
        byte_iter: Iterator over PDU bytes
        smsc_field: True if parsing SMSC address

    Returns:
        Tuple of (address_string, bytes_read)
        address_string is None if length is 0
    """
    address_len = next(byte_iter)

    if address_len == 0:
        return (None, 1)

    toa = next(byte_iter)
    ton = toa & 0x70  # Type of Number

    # Alphanumeric address (GSM-7 encoded)
    if ton == 0x50:
        address_len = math.ceil(address_len / 2)
        septets = septets_unpack(byte_iter, int(address_len))
        address_value = gsm7_decode(septets)
        return (address_value, int(address_len) + 2)

    # Numeric address (semi-octet encoded)
    if smsc_field:
        address_value = semi_octets_decode(byte_iter, address_len - 1)
    else:
        if address_len % 2:
            address_len = address_len // 2 + 1
        else:
            address_len = address_len // 2
        address_value = semi_octets_decode(byte_iter, int(address_len))
        address_len += 1

    # Add international prefix if applicable
    if ton == 0x10:  # International number
        address_value = "+" + address_value

    return (address_value, int(address_len) + 1)


def _build_address_field(address: str, smsc_field: bool = False) -> bytearray:
    """Build GSM address field from phone number string.

    Detects number type (international, national, alphanumeric) and
    encodes accordingly.

    Args:
        address: Phone number string
        smsc_field: True if building SMSC address

    Returns:
        Encoded address field
    """
    # Build Type-of-Address (TOA)
    toa = 0x80 | 0x01  # Unknown/International | ISDN numbering plan
    is_alphanumeric = False

    # Determine address type
    if address.isalnum():
        if address.isdigit():
            toa |= 0x20  # National number
        else:
            toa |= 0x50  # Alphanumeric
            toa &= 0xFE  # Unknown numbering plan
            is_alphanumeric = True
    else:
        if address.startswith("+") and address[1:].isdigit():
            toa |= 0x10  # International number
            address = address[1:]  # Remove '+' prefix
        else:
            toa |= 0x50  # Alphanumeric
            toa &= 0xFE
            is_alphanumeric = True

    # Encode address value
    if is_alphanumeric:
        address_value = septets_pack(gsm7_encode(address, discard_invalid=True))
        address_len = len(address_value) * 2
    else:
        address_value = semi_octets_encode(address)
        address_len = len(address_value) + 1 if smsc_field else len(address)

    # Build result
    result = bytearray()
    result.append(address_len)
    result.append(toa)
    result.extend(address_value)

    return result


# =============================================================================
# Semi-Octet Encoding
# =============================================================================

def semi_octets_encode(number: str) -> bytearray:
    """Encode string to semi-octets (swapped nibbles).

    Used for phone numbers and timestamps. Each pair of digits is
    byte-swapped (e.g., "1234" -> 0x21 0x43).

    Args:
        number: String of hex digits

    Returns:
        Encoded bytearray

    Example:
        >>> semi_octets_encode("1234")
        bytearray(b'!C')
        >>> semi_octets_encode("1234").hex()
        '2143'
    """
    if len(number) % 2 == 1:
        number = number + "F"  # Pad with filler

    octets = [int(number[i + 1] + number[i], 16) for i in range(0, len(number), 2)]
    return bytearray(octets)


def semi_octets_decode(
    encoded_number: Union[Iterator[int], bytes, bytearray],
    number_of_octets: Optional[int] = None
) -> str:
    """Decode semi-octets to string.

    Args:
        encoded_number: Iterator, bytes, or bytearray
        number_of_octets: Number of octets to decode (None = all)

    Returns:
        Decoded string (stops at 'F' if not specified)

    Example:
        >>> semi_octets_decode(bytes([0x21, 0x43]))
        '1234'
    """
    if number_of_octets == 0:
        return ""

    # Convert to iterator if needed
    if isinstance(encoded_number, (bytes, bytearray)):
        byte_iter = iter(encoded_number)
    else:
        byte_iter = encoded_number

    result: List[str] = []

    if number_of_octets is None:
        # Decode until 'F' or exhausted
        for octet in byte_iter:
            hex_val = f"{octet:02x}"
            result.append(hex_val[1])
            if hex_val[0] == "f":
                break
            result.append(hex_val[0])
    else:
        # Decode exactly number_of_octets
        for _ in range(number_of_octets):
            octet = next(byte_iter)
            hex_val = f"{octet:02x}"
            result.append(hex_val[1])
            if hex_val[0] == "f":
                break
            result.append(hex_val[0])

    return "".join(result)


# =============================================================================
# GSM-7 Encoding
# =============================================================================

def gsm7_encode(text: str, discard_invalid: bool = False) -> bytearray:
    """Encode text to GSM-7 septets (unpacked).

    Each character becomes a 7-bit value. Use septets_pack() to
    pack into bytes for transmission.

    Args:
        text: Unicode string to encode
        discard_invalid: Silently skip unencodable characters

    Returns:
        Bytearray of 7-bit values (0-127)

    Raises:
        ValueError: If character cannot be encoded (unless discard_invalid=True)

    Example:
        >>> gsm7_encode("Hello").hex()
        '386977c6'
    """
    result = bytearray()

    for char in text:
        idx = GSM7_BASIC.find(char)
        if idx != -1:
            result.append(idx)
        elif char in GSM7_EXTENDED:
            result.append(0x1B)  # Escape to extended table
            result.append(GSM7_EXTENDED[char])
        elif not discard_invalid:
            raise ValueError(f"Cannot encode character '{char}' (U+{ord(char):04X}) in GSM-7")

    return result


def gsm7_decode(encoded_text: Union[bytearray, str]) -> str:
    """Decode GSM-7 septets to Unicode string.

    Args:
        encoded_text: GSM-7 septets (bytes or bytearray)

    Returns:
        Decoded Unicode string

    Example:
        >>> gsm7_decode(bytes.fromhex('386977c6'))
        'Hello'
    """
    if isinstance(encoded_text, str):
        encoded_text = bytearray(encoded_text, "latin-1")

    result: List[str] = []
    iter_encoded = iter(encoded_text)

    for byte_val in iter_encoded:
        if byte_val == 0x1B:  # Escape character
            next_val = next(iter_encoded)
            for char, esc_val in GSM7_EXTENDED.items():
                if next_val == esc_val:
                    result.append(char)
                    break
        else:
            result.append(GSM7_BASIC[byte_val])

    return "".join(result)


def septets_pack(octets: Union[bytearray, Iterator[int]], pad_bits: int = 0) -> bytearray:
    """Pack GSM-7 septets into octets for transmission.

    Takes 7-bit values and packs them into 8-bit bytes, rotating
    bits to maximize space efficiency.

    Args:
        octets: Iterable of 7-bit values (0-127)
        pad_bits: Number of bits to skip initially (for UDH alignment)

    Returns:
        Packed bytearray

    Algorithm:
        Each iteration takes 7 bits from the current septet and
        combines with remaining bits from the previous septet.
    """
    result = bytearray()

    if isinstance(octets, (str, bytearray)):
        octets = iter(octets)

    shift = pad_bits
    if pad_bits == 0:
        prev_septet = next(octets)
    else:
        prev_septet = 0x00

    for octet in octets:
        septet = octet & 0x7F
        if shift == 7:
            shift = 0
            prev_septet = septet
            continue

        b = ((septet << (7 - shift)) & 0xFF) | (prev_septet >> shift)
        prev_septet = septet
        shift += 1
        result.append(b)

    if shift != 7:
        result.append(prev_septet >> shift)

    return result


def septets_unpack(
    septets: Union[Iterator[int], bytearray, str],
    number_of_septets: Optional[int] = None,
    prev_octet: Optional[int] = None,
    shift: int = 7
) -> bytearray:
    """Unpack GSM-7 septets from packed octets.

    Reverses the packing algorithm to extract 7-bit values.

    Args:
        septets: Packed octets
        number_of_septets: Number of septets to extract (None = all)
        prev_octet: Previous octet for continuation (internal use)
        shift: Initial shift value (internal use)

    Returns:
        Unpacked septets (values 0-127)
    """
    result = bytearray()

    if isinstance(septets, (str, bytearray)):
        if isinstance(septets, str):
            septets = bytearray(septets, "latin-1")
        septets = iter(septets)

    if number_of_septets is None:
        number_of_septets = sys.maxsize

    i = 0
    for octet in septets:
        i += 1

        if shift == 7:
            shift = 1
            if prev_octet is not None:
                result.append(prev_octet >> 1)

            if i <= number_of_septets:
                result.append(octet & 0x7F)
                prev_octet = octet

            if i == number_of_septets:
                break
            continue

        b = ((octet << shift) & 0x7F) | (prev_octet >> (8 - shift))  # type: ignore
        prev_octet = octet
        result.append(b)
        shift += 1

        if i == number_of_septets:
            break

    if shift == 7 and prev_octet is not None:
        b = prev_octet >> (8 - shift)
        if b:
            result.append(b)

    return result


# =============================================================================
# UCS-2 Encoding
# =============================================================================

def ucs2_encode(text: str) -> bytearray:
    """Encode text to UCS-2 (big-endian UTF-16).

    Args:
        text: Unicode string

    Returns:
        Big-endian UCS-2 bytes

    Example:
        >>> ucs2_encode("你好").hex()
        '4f60597d'
    """
    result = bytearray()
    for char in text:
        code = ord(char)
        result.append(code >> 8)     # High byte
        result.append(code & 0xFF)   # Low byte
    return result


def ucs2_decode(byte_iter: Iterator[int], num_bytes: int) -> str:
    """Decode UCS-2 text from byte iterator.

    Args:
        byte_iter: Iterator over bytes
        num_bytes: Number of bytes to decode (must be even)

    Returns:
        Decoded Unicode string
    """
    result: List[str] = []

    for _ in range(0, num_bytes, 2):
        try:
            high_byte = next(byte_iter)
            low_byte = next(byte_iter)
            result.append(chr((high_byte << 8) | low_byte))
        except StopIteration:
            break  # Incomplete pair

    return "".join(result)


# =============================================================================
# Utilities
# =============================================================================

def _as_bytearray(data: Union[str, bytes, bytearray]) -> bytearray:
    """Convert input to bytearray.

    Args:
        data: Hex string, bytes, or bytearray

    Returns:
        bytearray

    Raises:
        TypeError: If conversion fails
    """
    if isinstance(data, bytearray):
        return data
    if isinstance(data, bytes):
        return bytearray(data)
    if isinstance(data, str):
        try:
            return bytearray.fromhex(data)
        except ValueError:
            import codecs
            return bytearray(codecs.decode(data, "hex_codec"))
    raise TypeError(f"Cannot convert {type(data).__name__} to bytearray")
