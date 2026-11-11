"""Configure existing static ESP-NOW pairing for exactly one GEEK and two real STA MACs.
Does not scan, flash, connect, modify system settings or print keys.
All output paths are fixed below this checkout; existing local files abort.
"""
import argparse,secrets,re,json
from pathlib import Path

def mac(s):
 if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',s):raise argparse.ArgumentTypeError('STA MAC required: aa:bb:cc:dd:ee:ff')
 b=bytes.fromhex(s.replace(':',''))
 if b[0]&1 or not any(b):raise argparse.ArgumentTypeError('Nonzero unicast STA MAC required')
 return s.lower()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--geek-mac',type=mac,required=True);p.add_argument('--x-mac',type=mac,required=True);p.add_argument('--target-mac',type=mac,required=True);p.add_argument('--channel',type=int,choices=range(1,14),required=True);a=p.parse_args()
 if len({a.geek_mac,a.x_mac,a.target_mac})!=3:p.error('Three distinct real Wi-Fi STA MACs required')
 r=Path(__file__).resolve().parent.parent
 paths=[r/'ESP32-S3-GEEK/pairing.local.defaults',r/'x轴/esp_无线从机_20261022/pairing.local.defaults',r/'靶车/靶车/firmware/esp32_bridge/ble_direct/pairing.local.defaults']
 if any(x.exists() for x in paths):p.error('Existing pairing.local.defaults found; no overwrite. Back up/review existing pairing first.')
 pmk=secrets.token_hex(16);x_lmk=secrets.token_hex(16);x_auth=secrets.token_hex(32);t_lmk=secrets.token_hex(16);t_auth=secrets.token_hex(32)
 def config(node,peer,lmk,auth):return f'CONFIG_ROBOT_ESPNOW_ENABLED=y\nCONFIG_ROBOT_ESPNOW_NODE_ID={node}\nCONFIG_ROBOT_ESPNOW_CHANNEL={a.channel}\nCONFIG_ROBOT_ESPNOW_CONTROLLER_MAC="{peer}"\nCONFIG_ROBOT_ESPNOW_PMK="{pmk}"\nCONFIG_ROBOT_ESPNOW_LMK="{lmk}"\nCONFIG_ROBOT_ESPNOW_AUTH_KEY="{auth}"\n'
 configs=[config(1,a.x_mac,x_lmk,x_auth)+f'CONFIG_ROBOT_ESPNOW_TARGET_MAC="{a.target_mac}"\nCONFIG_ROBOT_ESPNOW_TARGET_LMK="{t_lmk}"\nCONFIG_ROBOT_ESPNOW_TARGET_AUTH_KEY="{t_auth}"\n',config(1,a.geek_mac,x_lmk,x_auth),config(2,a.geek_mac,t_lmk,t_auth)]
 for path in paths:
  if not path.parent.is_dir():p.error('Expected project missing: '+str(path.parent))
 for path,text in zip(paths,configs):
  with path.open('x',encoding='utf8') as f:f.write(text)
 print('Created 3 private pairing defaults; keys not printed. Rebuild all 3 using fresh SDKCONFIG plus sdkconfig.defaults;pairing.local.defaults. No device accessed.')
if __name__=='__main__':main()
