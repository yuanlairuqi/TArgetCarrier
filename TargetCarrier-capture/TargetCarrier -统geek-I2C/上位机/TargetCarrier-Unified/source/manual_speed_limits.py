"""Motor-shaft setpoint limits; shared generated data, not measured velocity."""
import json
from pathlib import Path
LIMITS=json.loads((Path(__file__).resolve().parent/'web/manual_speed_limits.json').read_text())
MANUAL_YAW_MAX_DPS=LIMITS['manual_yaw_max_dps']
WIRE_RATE_MAX_DPS=LIMITS['protocol_rate_max_dps']
