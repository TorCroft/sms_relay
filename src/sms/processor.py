"""SMS Message Processor

High-level SMS processing module that handles PDU decoding, concatenation,
and provides structured, easy-to-use results.

Example:
    >>> processor = SmsProcessor()
    >>> result = processor.process(pdu_hex)
    >>> if result.is_complete:
    ...     print(f"From: {result.sender}")
    ...     print(f"Text: {result.text}")
"""

from __future__ import annotations

import time
import threading
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional, List, Dict, Any, Union
from enum import Enum

from src.pdu.codec import decode_pdu
from src.pdu.codec import Concatenation


class MessageType(Enum):
    """SMS message type"""
    DELIVER = "SMS-DELIVER"
    SUBMIT = "SMS-SUBMIT"
    STATUS_REPORT = "SMS-STATUS-REPORT"


@dataclass
class SmsMessage:
    """Structured SMS message result.

    Attributes:
        sender: Sender phone number
        text: Complete message text (concatenated if multi-part)
        timestamp: Message timestamp (tz-aware)
        smsc: SMSC phone number
        type: Message type
        parts: Number of parts in concatenated message (1 if single)
        part_index: Current part index (1-indexed)
        is_complete: True if message is complete (all parts received)
        raw_pdu: Raw decoded PDU data
        ref: Concatenation reference number (None for single messages)
    """
    sender: str
    text: str
    timestamp: Optional[datetime] = None
    smsc: Optional[str] = None
    type: MessageType = MessageType.DELIVER
    parts: int = 1
    part_index: int = 1
    is_complete: bool = True
    raw_pdu: Optional[Dict[str, Any]] = None
    ref: Optional[int] = None

    def __repr__(self) -> str:
        if self.is_complete:
            return f"SmsMessage(from={self.sender}, text='{self.text[:30]}...')"
        return f"SmsMessage(from={self.sender}, part={self.part_index}/{self.parts})"


@dataclass
class PendingMessage:
    """Incomplete multi-part SMS being assembled.

    Attributes:
        sender: Sender phone number
        smsc: SMSC phone number
        timestamp: First part timestamp
        parts: Total number of parts
        ref: Concatenation reference number
        received_parts: Dict mapping part number to text
        created_at: Creation timestamp
        raw_pdu: Raw PDU data
    """
    sender: str
    smsc: Optional[str]
    timestamp: Optional[datetime]
    parts: int
    ref: int
    received_parts: Dict[int, str] = field(default_factory=dict)
    created_at: float = field(default_factory=time.time)
    raw_pdu: Optional[Dict[str, Any]] = None

    def add_part(self, index: int, text: str) -> bool:
        """Add a part and return True if complete.

        Args:
            index: Part number (1-indexed)
            text: Part text content

        Returns:
            True if all parts have been received
        """
        self.received_parts[index] = text
        return len(self.received_parts) >= self.parts

    def assemble(self) -> str:
        """Assemble complete message from all parts.

        Returns:
            Concatenated message text
        """
        return "".join(
            self.received_parts[i]
            for i in range(1, self.parts + 1)
            if i in self.received_parts
        )

    def is_expired(self, timeout: float) -> bool:
        """Check if pending message has expired.

        Args:
            timeout: Timeout in seconds

        Returns:
            True if message has expired
        """
        return time.time() - self.created_at > timeout


class SmsProcessor:
    """SMS message processor with automatic concatenation.

    Handles decoding PDUs, assembling multi-part messages, and
    providing structured results. Thread-safe for concurrent use.

    Example:
        >>> processor = SmsProcessor(timeout=60)
        >>>
        >>> # Process incoming PDUs
        >>> for pdu_hex in incoming_pdus:
        ...     result = processor.process(pdu_hex)
        ...     if result and result.is_complete:
        ...         handle_complete_message(result)
    """

    def __init__(self, timeout: float = 300, cleanup_interval: float = 60):
        """Initialize SMS processor.

        Args:
            timeout: Seconds to wait for incomplete messages (default: 5 minutes)
            cleanup_interval: Seconds between cleanup runs (default: 1 minute)
        """
        self._timeout = timeout
        self._cleanup_interval = cleanup_interval

        # Key: (sender, ref) -> PendingMessage
        self._pending: Dict[tuple, PendingMessage] = {}

        self._lock = threading.RLock()
        self._last_cleanup = time.time()

    def process(self, pdu: Union[str, bytes, bytearray]) -> Optional[SmsMessage]:
        """Process a single PDU and return structured message.

        Args:
            pdu: PDU hex string, bytes, or bytearray

        Returns:
            SmsMessage if valid PDU, None otherwise
        """
        # Run periodic cleanup
        self._maybe_cleanup()

        try:
            decoded = decode_pdu(pdu)
        except Exception:
            return None

        message_type = decoded.get("type", "")
        sender = decoded.get("number", "")
        text = decoded.get("text", "")

        # Only process deliver messages for concatenation
        if message_type != MessageType.DELIVER.value:
            return self._build_message(decoded, is_complete=True)

        # Check for UDH concatenation
        udh_list = decoded.get("udh", [])
        concat_ie = self._find_concatenation_ie(udh_list)

        if concat_ie is None:
            # Single message
            return self._build_message(decoded, is_complete=True)

        # Multi-part message - handle concatenation
        return self._process_concatenated(decoded, concat_ie)

    def process_batch(self, pdus: List[Union[str, bytes, bytearray]]) -> List[SmsMessage]:
        """Process multiple PDUs in batch.

        Args:
            pdus: List of PDU hex strings, bytes, or bytearrays

        Returns:
            List of SmsMessage objects (complete messages only)
        """
        complete_messages = []

        for pdu in pdus:
            result = self.process(pdu)
            if result and result.is_complete:
                complete_messages.append(result)

        return complete_messages

    def get_pending(self) -> List[SmsMessage]:
        """Get all currently pending (incomplete) messages.

        Returns:
            List of partial SmsMessage objects
        """
        with self._lock:
            partial_messages = []
            for pending in self._pending.values():
                for idx, part_text in pending.received_parts.items():
                    msg = SmsMessage(
                        sender=pending.sender,
                        text=part_text,
                        timestamp=pending.timestamp,
                        smsc=pending.smsc,
                        type=MessageType.DELIVER,
                        parts=pending.parts,
                        part_index=idx,
                        is_complete=False,
                        ref=pending.ref
                    )
                    partial_messages.append(msg)
            return partial_messages

    def cleanup(self) -> int:
        """Remove expired pending messages.

        Returns:
            Number of messages removed
        """
        with self._lock:
            expired_keys = [
                key for key, pending in self._pending.items()
                if pending.is_expired(self._timeout)
            ]

            for key in expired_keys:
                del self._pending[key]

            return len(expired_keys)

    def _find_concatenation_ie(self, udh_list: List) -> Optional[Concatenation]:
        """Find Concatenation IE in UDH list.

        Args:
            udh_list: List of InformationElement objects

        Returns:
            Concatenation object if found, None otherwise
        """
        for ie in udh_list:
            if isinstance(ie, Concatenation):
                return ie
        return None

    def _process_concatenated(
        self,
        decoded: Dict[str, Any],
        concat: Concatenation
    ) -> Optional[SmsMessage]:
        """Process concatenated message part.

        Args:
            decoded: Decoded PDU data
            concat: Concatenation IE

        Returns:
            SmsMessage if complete, None if still pending
        """
        sender = decoded.get("number", "")
        smsc = decoded.get("smsc")
        text = decoded.get("text", "")
        timestamp = decoded.get("time")

        ref = concat.reference
        total_parts = concat.parts
        part_index = concat.number

        # Validate concatenation data
        if total_parts == 0 or part_index == 0 or part_index > total_parts:
            # Invalid - return as single message
            return self._build_message(decoded, is_complete=True)

        key = (sender, ref)

        with self._lock:
            if key in self._pending:
                # Existing pending message
                pending = self._pending[key]
            else:
                # New pending message
                pending = PendingMessage(
                    sender=sender,
                    smsc=smsc,
                    timestamp=timestamp,
                    parts=total_parts,
                    ref=ref,
                    raw_pdu=decoded
                )
                self._pending[key] = pending

            # Add this part
            is_complete = pending.add_part(part_index, text)

            if is_complete:
                # Message complete - remove from pending
                del self._pending[key]

                return SmsMessage(
                    sender=sender,
                    text=pending.assemble(),
                    timestamp=pending.timestamp,
                    smsc=pending.smsc,
                    type=MessageType.DELIVER,
                    parts=total_parts,
                    part_index=total_parts,
                    is_complete=True,
                    raw_pdu=decoded,
                    ref=ref
                )

        # Return partial message info
        return SmsMessage(
            sender=sender,
            text=text,
            timestamp=timestamp,
            smsc=smsc,
            type=MessageType.DELIVER,
            parts=total_parts,
            part_index=part_index,
            is_complete=False,
            raw_pdu=decoded,
            ref=ref
        )

    def _build_message(
        self,
        decoded: Dict[str, Any],
        is_complete: bool = True
    ) -> SmsMessage:
        """Build SmsMessage from decoded PDU.

        Args:
            decoded: Decoded PDU data
            is_complete: Message completion status

        Returns:
            SmsMessage object
        """
        type_str = decoded.get("type", MessageType.DELIVER.value)
        try:
            message_type = MessageType(type_str)
        except ValueError:
            message_type = MessageType.DELIVER

        return SmsMessage(
            sender=decoded.get("number", ""),
            text=decoded.get("text", ""),
            timestamp=decoded.get("time"),
            smsc=decoded.get("smsc"),
            type=message_type,
            is_complete=is_complete,
            raw_pdu=decoded
        )

    def _maybe_cleanup(self) -> None:
        """Run cleanup if interval has elapsed."""
        now = time.time()
        if now - self._last_cleanup > self._cleanup_interval:
            self.cleanup()
            self._last_cleanup = now


# =============================================================================
# Convenience Functions
# =============================================================================

def process_sms(pdu: Union[str, bytes, bytearray]) -> Optional[SmsMessage]:
    """Quick single-PDU processing (stateless).

    This function creates a new processor for each call, so it cannot
    handle concatenation across multiple calls. Use SmsProcessor for
    handling multi-part messages.

    Args:
        pdu: PDU hex string, bytes, or bytearray

    Returns:
        SmsMessage if valid PDU, None otherwise

    Example:
        >>> msg = process_sms(pdu_hex)
        >>> if msg:
        ...     print(f"From: {msg.sender}")
        ...     print(f"Text: {msg.text}")
    """
    processor = SmsProcessor()
    return processor.process(pdu)


def process_sms_list(pdus: List[Union[str, bytes, bytearray]]) -> List[SmsMessage]:
    """Process list of PDUs with automatic concatenation.

    Args:
        pdus: List of PDU hex strings, bytes, or bytearrays

    Returns:
        List of complete SmsMessage objects

    Example:
        >>> pdus = [pdu1, pdu2, pdu3]  # Multiple parts
        >>> messages = process_sms_list(pdus)
        >>> for msg in messages:
        ...     print(f"{msg.sender}: {msg.text}")
    """
    processor = SmsProcessor()
    return processor.process_batch(pdus)
