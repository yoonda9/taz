"""Smoke tests for the generated protobuf modules (no daemon required)."""

from taz.v1 import advanced_pb2, command_pb2, common_pb2, daemon_control_pb2


def test_opcode_values_match_specification() -> None:
    assert common_pb2.OPCODE_PING == 0x0001
    assert common_pb2.OPCODE_COMMAND_EXEC == 0x0010
    assert common_pb2.OPCODE_PROCESS_LIST == 0x0020
    assert common_pb2.OPCODE_FILE_PUT == 0x0030
    assert common_pb2.OPCODE_RUN_AS == 0x0040
    assert common_pb2.OPCODE_CANCEL == 0x0046
    assert common_pb2.OPCODE_PIPELINE == 0x0050


def test_error_code_values_match_specification() -> None:
    assert common_pb2.ErrorCode.Name(0) == "ERROR_CODE_UNKNOWN"
    assert common_pb2.ErrorCode.Name(1) == "ERROR_CODE_NOT_FOUND"
    assert common_pb2.ErrorCode.Name(10) == "ERROR_CODE_CONNECTION_LOST"


def test_frame_constants() -> None:
    assert common_pb2.FRAME_TYPE_REQUEST == 0x01
    assert common_pb2.FRAME_TYPE_CAPABILITY == 0x07
    assert common_pb2.FRAME_FLAG_CONTINUATION | common_pb2.FRAME_FLAG_PRIORITY == 0x05


def test_error_info_round_trip() -> None:
    message = common_pb2.ErrorInfo(
        code=common_pb2.ERROR_CODE_NOT_FOUND, message="no such file", detail="/x"
    )
    decoded = common_pb2.ErrorInfo.FromString(message.SerializeToString())
    assert decoded == message


def test_capability_payload_round_trip() -> None:
    payload = daemon_control_pb2.CapabilityPayload(
        protocol_major=1,
        protocol_minor=0,
        operations=[common_pb2.OPCODE_VERSION, common_pb2.OPCODE_COMMAND_EXEC],
        max_payload_sizes=[
            common_pb2.UInt32Pair(key=common_pb2.FRAME_TYPE_FILE_CHUNK, value=65536)
        ],
        compression=["NONE"],
    )
    raw = payload.SerializeToString()
    assert len(raw) <= 1024
    assert daemon_control_pb2.CapabilityPayload.FromString(raw) == payload


def test_detach_oneof_wraps_command_exec() -> None:
    request = advanced_pb2.DetachRequest(
        exec=command_pb2.CommandExecRequest(command="sleep", args=["10"]),
        monitor=True,
        error_priority=common_pb2.ERROR_PRIORITY_CRITICAL,
    )
    assert request.WhichOneof("task") == "exec"
    decoded = advanced_pb2.DetachRequest.FromString(request.SerializeToString())
    assert decoded.exec.args == ["10"]
