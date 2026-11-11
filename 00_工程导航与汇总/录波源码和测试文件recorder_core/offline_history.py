"""Prototype offline reader for CR/1 records, not connected to a GEEK SD card."""
from __future__ import annotations
import argparse,csv,json,struct,zlib
from pathlib import Path
RECORD=40
def records(path):
    with Path(path).open('rb') as f:
        index=0
        while True:
            b=f.read(RECORD)
            if not b:break
            if len(b)!=RECORD:raise ValueError(f'truncated record at {index}')
            if b[:3]!=b'CR\x01' or b[5] or b[10:12]!=b'\x00\x00' or zlib.crc32(b[:36])!=struct.unpack_from('<I',b,36)[0]:raise ValueError(f'corrupt record at {index}')
            kind,typ=b[3],b[4];channel,unit=struct.unpack_from('<HH',b,6);source,boot,seq=struct.unpack_from('<III',b,12);ts=struct.unpack_from('<Q',b,24)[0];bits=struct.unpack_from('<I',b,32)[0]
            if kind not in (1,2,3) or typ not in (1,2,3) or not source or not boot:raise ValueError(f'invalid record at {index}')
            value=bits if typ==1 else struct.unpack('<i' if typ==2 else '<f',struct.pack('<I',bits))[0]
            yield dict(kind=kind,type=typ,channel=channel,unit=unit,source=source,boot=boot,sequence=seq,sample_us=ts,raw_bits=f'{bits:08x}',value=value)
            index+=1
def export(src,dest,source=None,start=None,end=None):
    src,dest=Path(src).resolve(),Path(dest).resolve()
    if src==dest:raise ValueError('destination must differ from original')
    with dest.open('x',encoding='utf-8-sig',newline='') as out:
        writer=csv.DictWriter(out,fieldnames=['kind','type','channel','unit','source','boot','sequence','sample_us','raw_bits','value']);writer.writeheader()
        for row in records(src):
            if source is not None and row['source']!=source:continue
            if start is not None and row['sample_us']<start:continue
            if end is not None and row['sample_us']>end:continue
            writer.writerow(row)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('input');p.add_argument('csv');p.add_argument('--source',type=int);p.add_argument('--start-us',type=int);p.add_argument('--end-us',type=int);a=p.parse_args();export(a.input,a.csv,a.source,a.start_us,a.end_us)
if __name__=='__main__':main()
