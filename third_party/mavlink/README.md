# MAVLink C library (vendored)

The official generated MAVLink 2 C library, header-only, used by `src/vehicle/mav_vehicle.cpp` to
pack and parse the messages of the boat-mission feature (heartbeat, telemetry, mission protocol,
commands).

| | |
|---|---|
| Source | <https://github.com/mavlink/c_library_v2> |
| Commit | `c1fd65eb702106097a4253c259aba34a18235848` (2026-09-24), generated from mavlink/mavlink `dc252db7` |
| What is here | the top-level headers (`checksum.h`, `mavlink_helpers.h`, `mavlink_types.h`, `protocol.h`, `mavlink_conversions.h`, `mavlink_get_info.h`, `mavlink_sha256.h`) and the dialects `common/`, `standard/`, `minimal/` unmodified |
| Not here | other dialects (`ardupilotmega`, ...): ArduPilot's own messages are not needed, and the library drops unknown ids on the CRC check |
| Licence | MIT, see [LICENSE-MIT.txt](LICENSE-MIT.txt) |

To update: copy the same files from a newer c_library_v2 checkout over these and rebuild. The
app's own frame recogniser (`src/mav_link_conf.h`, CRC_EXTRA table) is independent of this copy;
the ids the feature needs were checked against it on 2026-09-26 and all matched.
