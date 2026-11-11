"""One COM owner, bounded per-node mailboxes. No device is opened on import."""
import threading,time,queue,struct
class SerialHub:
    registry={};registry_lock=threading.RLock()
    @classmethod
    def acquire(cls,port,node,factory,parser):
        with cls.registry_lock:
            hub=cls.registry.get(port)
            if hub is None:
                hub=cls(port,factory,parser);cls.registry[port]=hub
            if hub.error:raise IOError(hub.error)
            if node in hub.mail:raise RuntimeError('同一 GEEK 节点已被占用')
            box=(queue.Queue(32),queue.Queue(8))
            with hub.lock:hub.mail[node]=box
            return hub,box
    def __init__(self,port,factory,parser):
        self.port=port;self.mail={};self.lock=threading.RLock();self.write_lock=threading.Lock()
        self.capture_mail=set();self.capture_drops=0
        self.error=None;self.drops=0;self.running=True;self.parser=parser
        self.serial=factory(port,115200,timeout=0,write_timeout=.1)
        for method in ('reset_input_buffer','reset_output_buffer'):
            if hasattr(self.serial,method):getattr(self.serial,method)()
        self.thread=threading.Thread(target=self.reader,name='geek-usb-owner',daemon=True);self.thread.start()
    def capture_subscribe(self,capacity=128):
        """Receive future optional recording frames through the existing port owner.

        An observer never owns or prolongs the connection, and cannot block control.
        """
        if not isinstance(capacity,int) or capacity < 1:raise ValueError('invalid capture capacity')
        q=queue.Queue(capacity)
        with self.lock:self.capture_mail.add(q)
        return q
    def capture_unsubscribe(self,q):
        with self.lock:self.capture_mail.discard(q)
    def route(self,kind,body):
        if kind in (4,7):
            with self.lock:
                for q in tuple(self.capture_mail):
                    try:q.put_nowait((time.monotonic(),kind,bytes(body)))
                    except queue.Full:self.capture_drops+=1
        if kind not in (2,4,5):return
        node=struct.unpack_from('<I',body,4)[0]
        with self.lock:
            boxes=list(self.mail.values()) if kind==5 else ([self.mail[node]] if node in self.mail else [])
            for control,wave in boxes:
                q=wave if kind==4 else control
                try:q.put_nowait((time.monotonic(),kind,body))
                except queue.Full:
                    try:q.get_nowait()
                    except queue.Empty:pass
                    q.put_nowait((time.monotonic(),kind,body));self.drops+=1
    def reader(self):
        try:
            while self.running:
                data=self.serial.read(1152)
                for kind,body in self.parser.feed(data):self.route(kind,body)
                time.sleep(.002)
        except Exception as exc:self.error=str(exc);self.running=False
    def write(self,packet,created):
        with self.write_lock:
            if self.error or not self.running:raise IOError(self.error or 'USB 已关闭')
            if time.monotonic()-created>.1:return 0
            n=self.serial.write(packet)
            if n!=len(packet):raise IOError('USB 写入不完整；不重放命令')
            return n
    def release(self,node):
        with self.registry_lock:
            with self.lock:self.mail.pop(node,None);last=not self.mail
            if last:
                self.running=False;self.thread.join(timeout=.5);self.serial.close()
                if self.registry.get(self.port) is self:self.registry.pop(self.port)
