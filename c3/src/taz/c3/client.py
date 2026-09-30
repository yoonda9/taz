"""TazClient: high-level API for the TAZ daemon."""

from __future__ import annotations

from types import TracebackType

from taz.c3.connection import Connection
from taz.c3.errors import TazError
from taz.c3.protocol.dispatch import Dispatcher
from taz.c3.protocol.frame import Frame
from taz.c3.settings import Backlog, Keepalive
from taz.v1 import common_pb2, daemon_control_pb2


def _parse_error(frame: Frame) -> None:
    """Raise TazError from an ERROR frame."""
    info = common_pb2.ErrorInfo()
    info.ParseFromString(frame.payload)
    raise TazError(info.code, info.message, info.detail)


class TazClient:
    """High-level client for the TAZ daemon.

    Usage::

        with TazClient("127.0.0.1", 5555) as client:
            client.ping()
            info = client.version()
    """

    def __init__(
        self,
        host: str,
        port: int,
        *,
        timeout: float | None = None,
        keepalive: Keepalive | None = None,
        backlog: Backlog | None = None,
    ) -> None:
        self._host = host
        self._port = port
        self._keepalive = keepalive if keepalive is not None else Keepalive()
        self._conn = Connection(timeout=timeout)
        self._dispatcher = Dispatcher(self._conn, backlog)

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def connect(self) -> None:
        """Open the TCP connection and complete the CAPABILITY handshake."""
        self._conn.connect(self._host, self._port)

    def close(self) -> None:
        """Close the underlying socket."""
        self._conn.close()

    def __enter__(self) -> TazClient:
        self.connect()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _call(
        self,
        opcode: int,
        payload: bytes,
        keepalive: Keepalive | None,
    ) -> Frame:
        kv = keepalive if keepalive is not None else self._keepalive
        stream_id = self._conn.send_request(opcode, payload)
        try:
            frame = self._dispatcher.recv_response(
                stream_id, kv, expected_opcode=opcode
            )
        except BaseException as exc:
            # wait_readable may raise outside Connection._io so ensure the
            # connection is closed and the failure recorded here too.
            if self._conn._failure is None:
                self._conn._failure = exc
            self._conn.close()
            raise
        if frame.type == common_pb2.FRAME_TYPE_ERROR:
            _parse_error(frame)
        return frame

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def ping(self, keepalive: Keepalive | None = None) -> None:
        """Send a FRAME_TYPE_PING and wait for FRAME_TYPE_PONG."""
        self._conn._check_failure()
        kv = keepalive if keepalive is not None else self._keepalive
        # _send_ping wraps the send in _io(), handling send-side failures.
        self._conn._send_ping()
        # recv_pong may raise via wait_readable outside _io, so record here.
        try:
            self._dispatcher.recv_pong(kv)
        except BaseException as exc:
            if self._conn._failure is None:
                self._conn._failure = exc
            self._conn.close()
            raise

    def version(
        self, keepalive: Keepalive | None = None
    ) -> daemon_control_pb2.VersionResponse:
        """Return the daemon's version information."""
        req = daemon_control_pb2.VersionRequest()
        frame = self._call(
            common_pb2.OPCODE_VERSION, req.SerializeToString(), keepalive
        )
        resp = daemon_control_pb2.VersionResponse()
        resp.ParseFromString(frame.payload)
        return resp

    def capabilities(self) -> daemon_control_pb2.CapabilityPayload:
        """Return the CAPABILITY payload received during the handshake."""
        return self._conn.capabilities

    def config_get(
        self,
        keys: list[str] | None = None,
        keepalive: Keepalive | None = None,
    ) -> dict[str, str]:
        """Return the daemon's current configuration.

        If ``keys`` is given, only those keys are returned.  Unknown keys are
        silently omitted.  With no ``keys`` argument all keys are returned.
        """
        req = daemon_control_pb2.ConfigurationGetRequest()
        if keys:
            req.keys.extend(keys)
        frame = self._call(
            common_pb2.OPCODE_CONFIGURATION_GET,
            req.SerializeToString(),
            keepalive,
        )
        resp = daemon_control_pb2.ConfigurationGetResponse()
        resp.ParseFromString(frame.payload)
        return {kv.key: kv.value for kv in resp.config}

    def config_update(
        self,
        config: dict[str, str],
        keepalive: Keepalive | None = None,
    ) -> daemon_control_pb2.ConfigurationUpdateResponse:
        """Apply ``config`` key/value pairs to the daemon configuration.

        Returns the full ``ConfigurationUpdateResponse`` so callers can inspect
        ``applied`` and ``rejected`` lists.
        """
        req = daemon_control_pb2.ConfigurationUpdateRequest()
        for key, value in config.items():
            kv = req.config.add()
            kv.key = key
            kv.value = value
        frame = self._call(
            common_pb2.OPCODE_CONFIGURATION_UPDATE,
            req.SerializeToString(),
            keepalive,
        )
        resp = daemon_control_pb2.ConfigurationUpdateResponse()
        resp.ParseFromString(frame.payload)
        return resp
