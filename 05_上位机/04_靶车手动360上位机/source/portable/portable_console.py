"""Standalone TC/2 console beside the preserved Vue control console."""
import tkinter as tk
from tkinter import ttk,messagebox
import time,struct,queue,collections,csv
from tc_protocol import Client,READ,WRITE
def main():
 app=tk.Tk();app.title('步兵通用调参 · TC/2');app.geometry('1050x720');client=Client();points=collections.deque(maxlen=2000)
 top=ttk.Frame(app,padding=12);top.pack(fill='x');ttk.Label(top,text='GEEK USB 端口').pack(side='left');port=tk.StringVar();ports=ttk.Combobox(top,textvariable=port,width=16);ports.pack(side='left',padx=6)
 def refresh():
  try:
   from serial.tools.list_ports import comports
   ports['values']=[p.device for p in comports()]
  except ImportError:messagebox.showerror('缺少依赖','请安装 pyserial，或运行已打包 EXE')
 def connect():
  if not port.get():return
  try:client.connect(port.get())
  except Exception as e:messagebox.showerror('连接',str(e))
 ttk.Button(top,text='列出端口',command=refresh).pack(side='left');ttk.Button(top,text='连接',command=connect).pack(side='left',padx=6);ttk.Button(top,text='断开',command=client.disconnect).pack(side='left')
 ttk.Label(top,text='固件后端预期').pack(side='left',padx=(22,4));backend=tk.StringVar(value='UART');ttk.Combobox(top,textvariable=backend,values=['UART','SPI','I²C'],state='readonly',width=8).pack(side='left')
 ttk.Label(app,text='后端由编译配置选择；此选择仅用于核对，不能在线更换接线或固件。参考波形是软件演示数据。',padding=8).pack(anchor='w')
 state=tk.StringVar(value='未连接');ttk.Label(app,textvariable=state,padding=8).pack(anchor='w')
 form=ttk.LabelFrame(app,text='参数读写（参考参数：1=U32 / 1..1000；2=F32 / 0..100）',padding=12);form.pack(fill='x',padx=12)
 pid=tk.StringVar(value='1');typ=tk.StringVar(value='U32');value=tk.StringVar(value='25');ttk.Label(form,text='ID').pack(side='left');ttk.Entry(form,textvariable=pid,width=9).pack(side='left',padx=6);ttk.Combobox(form,textvariable=typ,values=['U32','I32','F32'],state='readonly',width=8).pack(side='left');ttk.Entry(form,textvariable=value,width=16).pack(side='left',padx=6)
 def request(write):
  try:
   ident=int(pid.get(),0)
   if not 0<=ident<=65535:raise ValueError('参数 ID 范围 0..65535')
   d=struct.pack('<H',ident)
   if write:
    t={'U32':1,'I32':2,'F32':3}[typ.get()];v=float(value.get()) if t==3 else int(value.get(),0);d+=bytes([t,0])+struct.pack('<'+{1:'I',2:'i',3:'f'}[t],v)
   if not client.request(WRITE if write else READ,d):messagebox.showinfo('连接','尚未连接或请求队列已满')
  except (ValueError,OverflowError,struct.error) as e:messagebox.showerror('参数',str(e))
 ttk.Button(form,text='读取',command=lambda:request(False)).pack(side='left');ttk.Button(form,text='写入',command=lambda:request(True)).pack(side='left',padx=6)
 result=tk.StringVar(value='等待应答');ttk.Label(app,textvariable=result,padding=8).pack(anchor='w')
 canvas=tk.Canvas(app,bg='#101c2b',highlightthickness=0);canvas.pack(fill='both',expand=True,padx=12,pady=8)
 note=tk.StringVar(value='等待完整波形');ttk.Label(app,textvariable=note,padding=8).pack(anchor='w')
 def tick():
  for _ in range(128):
   try:e=client.events.get_nowait()
   except queue.Empty:
    try:e=client.waves.get_nowait()
    except queue.Empty:break
   if e[0]=='wave':
    d=e[1]
    for k in range(d['samples']):points.append(((d['first_us']+k*d['period_us'])&0xffffffff,d['ids'],d['values'][k*len(d['ids']):(k+1)*len(d['ids'])]))
    note.set(f"通道 {d['ids']} · 周期 {d['period_us']} µs · 有界缓存 {len(points)}/2000 · 接收事件丢弃 {client.drops}")
   elif e[0]=='result':
    d=e[1];msg=f'类型 {d[0]} · 结果 {d[1]} · 阶段 '+('已应用' if d[7]==1 else '未应用/握手')
    if len(d)>=12:
     ident=struct.unpack_from('<H',d,2)[0];t=d[4]
     if t in (1,2,3):msg+=f" · 参数 {ident} = {struct.unpack_from('<'+{1:'I',2:'i',3:'f'}[t],d,8)[0]}"
    result.set(msg)
   elif e[0]=='error':result.set(e[1])
  now=time.monotonic();target=client.connected and now-client.last_target<1.5;radio=client.connected and now-client.last_peer<2;reported={1:'UART',2:'SPI',3:'I²C'}.get(client.peer_backend,'未知')
  state.set(f"USB：{'已打开' if client.connected else '未连接'}　无线：{'接收端状态新鲜' if radio else '未知/未配对/超时'}（接收端后端：{reported}，{'匹配' if reported==backend.get() else '未匹配'}）　STM32：{'应答新鲜' if target else '未确认'}　迟到应答：{client.late}")
  canvas.delete('all');w=max(1,canvas.winfo_width());h=max(1,canvas.winfo_height());series=list(points)[-300:]
  if series:
   channels=sorted(set(c for _,ids,_ in series for c in ids))[:8];colors=['#35d0af','#f2b84b','#8787ff','#fa7189','#55b9ff','#eee','#bcef63','#e4acfa']
   for i,ch in enumerate(channels):
    v=[(k,values[ids.index(ch)]) for k,(_,ids,values) in enumerate(series) if ch in ids]
    lo=min(x[1] for x in v);hi=max(x[1] for x in v);coords=[]
    for k,y in v:coords.extend((35+k/max(1,len(series)-1)*(w-60),h-35-(y-lo)/max(1e-12,hi-lo)*(h-70)))
    if len(coords)>=4:canvas.create_line(*coords,fill=colors[i],width=2)
    canvas.create_text(45+i*120,15,text=f'CH{ch}: {v[-1][1]:.4g}',fill=colors[i],anchor='w')
  app.after(100,tick)
 def close():client.disconnect();app.destroy()
 app.protocol('WM_DELETE_WINDOW',close);app.after(100,tick)
 import sys,json
 if '--ui-smoke' in sys.argv:
  def smoke():
   Path=__import__('pathlib').Path;Path('portable-ui-smoke.json').write_text(json.dumps(dict(title=app.title(),widgets=len(app.winfo_children()),connected=client.connected,port=port.get(),parameter_id=pid.get(),backend=backend.get()),ensure_ascii=False,indent=2),encoding='utf8');close()
  app.after(800,smoke)
 app.mainloop()
if __name__=='__main__':main()
