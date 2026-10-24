"""Create fresh local pairing defaults for BOTH copies, never touch originals.
No serial access, flashing, or system configuration. Missing real STA MACs abort.
"""
import argparse
import json
import re
import secrets
from pathlib import Path

def mac(value):
    if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',value):raise argparse.ArgumentTypeError('STA MAC format aa:bb:cc:dd:ee:ff required')
    raw=bytes.fromhex(value.replace(':',''))
    if raw[0]&1 or not any(raw):raise argparse.ArgumentTypeError('nonzero unicast STA MAC required')
    return value.lower()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--master-mac',required=True,type=mac);p.add_argument('--slave-mac',required=True,type=mac)
    p.add_argument('--slave-copy',required=True,type=Path);p.add_argument('--pc-copy',required=True,type=Path)
    p.add_argument('--node',type=int,default=1);p.add_argument('--channel',type=int,required=True)
    a=p.parse_args();master=Path(__file__).resolve().parent;slave=a.slave_copy.resolve();pc=a.pc_copy.resolve()
    if not 1<=a.node<=65535 or not 1<=a.channel<=13:p.error('node 1..65535, channel 1..13')
    if a.master_mac==a.slave_mac:p.error('master and slave STA MACs must differ')
    if not (slave/'WIRELESS_COPY_MARKER').is_file():p.error('slave-copy must be the explicitly marked compatibility copy')
    if not (pc/'WIRELESS_COPY_MARKER').is_file():p.error('pc-copy must be the explicitly marked PC copy')
    paths=[master/'pairing.local.defaults',slave/'pairing.local.defaults',pc/'usb.local.json']
    if any(x.exists() for x in paths):p.error('local pairing/config already exists; no files overwritten')
    pmk,lmk,auth=secrets.token_hex(16),secrets.token_hex(16),secrets.token_hex(32)
    common=f'''CONFIG_ROBOT_ESPNOW_ENABLED=y
CONFIG_ROBOT_ESPNOW_NODE_ID={a.node}
CONFIG_ROBOT_ESPNOW_CHANNEL={a.channel}
CONFIG_ROBOT_ESPNOW_PMK="{pmk}"
CONFIG_ROBOT_ESPNOW_LMK="{lmk}"
CONFIG_ROBOT_ESPNOW_AUTH_KEY="{auth}"
'''
    for path,peer in zip(paths[:2],[a.slave_mac,a.master_mac]):
        with path.open('x',encoding='utf8') as f:f.write(common+f'CONFIG_ROBOT_ESPNOW_CONTROLLER_MAC="{peer}"\n')
    with paths[2].open('x',encoding='utf8') as f:json.dump(dict(node=a.node),f)
    print('Created local pairing files; keys are not printed. Rebuild with a fresh SDKCONFIG and both defaults files. No device accessed.')
if __name__=='__main__':main()
