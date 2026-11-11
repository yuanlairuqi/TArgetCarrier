import cn.robot.ble.Protocol;
import cn.robot.ble.ReceiveHistory;

public class ReceiveHistoryTest {
    private static byte[] frame(int flags) {
        byte[] b=new byte[20];b[0]=3;b[1]=2;b[2]=(byte)flags;
        b[3]=(byte)0xe8;b[4]=0x1c;b[5]=1;b[6]=55;
        for(int i=8;i<16;i++)b[i]=(byte)255;
        return b;
    }
    public static void main(String[] args) {
        ReceiveHistory history=new ReceiveHistory(3,4);
        assert history.snapshot(0).frames.isEmpty();
        history.connection(true,"已订阅",1000);
        byte[] b=frame(1|4|8|16|32|128);
        Protocol.Status status=history.receive(100,1000,b,-61);
        assert status!=null&&status.ack==0xffffffffL&&status.stmAck==0xffffffffL;
        ReceiveHistory.Snapshot frozen=history.snapshot(100);
        ReceiveHistory.Frame first=frozen.frames.get(0);
        assert first.value(0)==55&&first.value(1)==7.4&&first.value(2)==-61;
        assert first.value(3)==1&&first.value(4)==1&&first.value(5)==2;
        b[0]=0;b[6]=0;
        assert first.hex().startsWith("03 02")&&first.status.speed==55;
        assert history.receive(200,1100,b,Double.NaN)==null;
        ReceiveHistory.Frame bad=history.snapshot(200).frames.get(1);
        for(int i=0;i<6;i++)assert Double.isNaN(bad.value(i));
        assert history.snapshot(200).invalid==1&&history.snapshot(200).received==2;
        byte[] offline=frame(1|128);offline[3]=(byte)255;offline[4]=(byte)255;
        history.receive(300,1200,offline,Double.NaN);
        ReceiveHistory.Frame missing=history.snapshot(300).frames.get(2);
        assert Double.isNaN(missing.value(1))&&Double.isNaN(missing.value(2));
        assert missing.value(3)==0&&Double.isNaN(missing.value(4))&&Double.isNaN(missing.value(5));
        int oldSegment=missing.segment;
        history.connection(false,"断开",1300);
        history.connection(true,"重新订阅",1400);
        history.receive(3000,1500,frame(1|128),-65);
        ReceiveHistory.Snapshot latest=history.snapshot(3000);
        assert latest.frames.size()==3&&latest.received==4;
        assert latest.frames.get(2).segment!=oldSegment;
        assert latest.recentFrames==1;
        assert frozen.frames.size()==1&&frozen.received==1&&frozen.latest==first;
        boolean immutable=false;
        try{frozen.frames.clear();}catch(UnsupportedOperationException e){immutable=true;}
        assert immutable;
        history.receive(3100,1600,frame(1|2),-70);
        ReceiveHistory.Frame demo=history.snapshot(3100).latest;
        assert Double.isNaN(demo.value(3))&&Double.isNaN(demo.value(5));
        history.receive(3200,1700,frame(1|8|16|32),-60);
        assert Double.isNaN(history.snapshot(3200).latest.value(4));
        assert Double.isNaN(history.snapshot(3200).latest.value(5));
        long revision=history.snapshot(3200).revision;
        for(int i=0;i<20;i++)history.event("event "+i,2000+i);
        assert history.snapshot(3200).revision>revision;
        assert !history.snapshot(3100).log.contains("event 15");
        assert history.snapshot(3100).log.contains("event 19");
        history.clear();
        assert history.snapshot(4000).received==0&&history.snapshot(4000).latest==null;
        assert history.snapshot(4000).connected&&history.snapshot(4000).frames.isEmpty();
        assert history.snapshot(4000).log.isEmpty();
        byte[] sentinel=frame(1|4|128);sentinel[3]=(byte)255;sentinel[4]=(byte)255;
        history.receive(4100,3000,sentinel,-50);
        assert Double.isNaN(history.snapshot(4100).latest.value(1));
        assert ReceiveHistory.GAP_MS==1500;
        System.out.println("PASS: received telemetry bounds, immutable snapshots, raw bytes, invalid gaps, offline/demo gating, reconnect segments, unsigned ACK and clear");
    }
}
