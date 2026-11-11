from pathlib import Path

import json,argparse

r=Path(__file__).resolve().parents[1];c=json.loads((r/'config/manual_speed_limits.json').read_text())

assert 360<=c['manual_yaw_max_dps']<=c['protocol_rate_max_dps']<=3276.7

outputs={r/'firmware/lib/protocol/manual_speed_limits.h':'/* Generated from config/manual_speed_limits.json; run tools/generate_manual_speed_limits.py */\n#ifndef MANUAL_SPEED_LIMITS_H\n#define MANUAL_SPEED_LIMITS_H\n'+''.join('#define '+k+' '+str(c[v])+'f\n' for k,v in [('MANUAL_YAW_MAX_DPS','manual_yaw_max_dps'),('TRAJECTORY_YAW_MAX_DPS','trajectory_yaw_max_dps'),('WIRE_RATE_MAX_DPS','protocol_rate_max_dps')])+'#endif\n'}

p=argparse.ArgumentParser();p.add_argument('--check',action='store_true');a=p.parse_args()

for path,text in outputs.items():

 if a.check:assert path.read_text()==text,str(path)

 else:path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)

