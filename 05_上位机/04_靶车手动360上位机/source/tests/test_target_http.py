"""Loopback-only tests of the production HTTP client; never contact real boards."""
import json
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
from urllib.error import HTTPError
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from target_adapter import HttpClient, TargetAdapter


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calls=[]
        cls.state=dict(en=0,estop=0,up=100,yaw=0,pitch=0,st_age=10,st_err=0,st_state=1,kp=40,kd=1.5)
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                p=urlsplit(self.path)
                if p.path=='/redirect':
                    self.send_response(302);self.send_header('Location','/api/cmd?c=E');self.end_headers();return
                if p.path=='/oversize':
                    body=b' '*16385
                elif p.path=='/nan':body=b'{"en":NaN}'
                elif p.path=='/api/status':body=json.dumps(cls.state).encode()
                elif p.path=='/api/cmd':
                    cmd=parse_qs(p.query)['c'][0];cls.calls.append(cmd)
                    if cmd=='x':cls.state['estop']=1
                    if cmd=='C':cls.state['en']=0
                    body=b'{"ok":1}'
                else:self.send_error(404);return
                self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        cls.thread=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.thread.start()
        cls.endpoint='http://127.0.0.1:'+str(cls.server.server_port)
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown();cls.server.server_close();cls.thread.join()
    def test_production_connect_routes_stop_and_status(self):
        a=TargetAdapter(background=False);a.connect(self.endpoint);a.tick()
        self.assertTrue(a.snapshot()['connected'])
        self.assertEqual(self.calls[-2:],['x','C'])
        self.assertFalse(a.snapshot()['physical_stop_confirmed'])
    def test_command_encoding(self):
        HttpClient()(self.endpoint,'/api/cmd?c=y%20-12.5')
        self.assertEqual(self.calls[-1],'y -12.5')
    def test_redirect_is_rejected(self):
        count=len(self.calls)
        with self.assertRaises(HTTPError):HttpClient()(self.endpoint,'/redirect')
        self.assertEqual(count,len(self.calls))
    def test_oversize_and_nonfinite_rejected(self):
        for path in ('/oversize','/nan'):
            with self.subTest(path=path),self.assertRaises(ValueError):HttpClient()(self.endpoint,path)


if __name__=='__main__':unittest.main()
