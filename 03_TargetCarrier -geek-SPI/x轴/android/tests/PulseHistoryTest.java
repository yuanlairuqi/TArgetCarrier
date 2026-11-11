import cn.robot.ble.PulseHistory;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.zip.CRC32;
public class PulseHistoryTest {
    private static byte[] packet(byte[] f,int n){return Arrays.copyOfRange(f,n*20,n*20+20);}
    private static void feed(PulseHistory h,byte[] f){for(int i=0;i<f.length/20;i++)h.receive(packet(f,i),1000+i);}
    public static void main(String[] args)throws Exception{
        byte[] f=Files.readAllBytes(Paths.get(args[0]));PulseHistory h=new PulseHistory();h.connection(true,true);feed(h,f);
        PulseHistory.Capture c=h.latest();assert c!=null&&c.complete()&&c.count()==21&&c.id==65535&&c.initial==0;
        assert c.durationMs()==100&&c.pulGpio==6&&c.dirGpio==7;
        assert c.measurements().contains("100.000 Hz")&&c.measurements().contains("5.000");assert c.csv().contains("DIR,1200000,15.000000,1");
        h.clear();h.receive(packet(f,0),0);h.receive(packet(f,2),1);assert h.latest()==null&&h.state().contains("缺包");
        h.connection(true,true);byte[] corrupt=f.clone();corrupt[25]^=1;feed(h,corrupt);assert h.latest()==null&&h.state().contains("校验");
        h.connection(true,true);h.receive(packet(f,0),0);h.receive(packet(f,1),1);h.receive(packet(f,1),2);assert h.latest()==null;
        h.connection(true,true);byte[] oversized=packet(f,0);oversized[14]=1;oversized[15]=3;h.receive(oversized,0);assert h.latest()==null;
        h.connection(true,true);feed(h,f);h.connection(false,false);h.receive(packet(f,0),2000);assert h.latest()==c||h.latest().id==c.id;assert h.state().contains("断线");
        h.connection(true,false);assert h.state().contains("更新");h.clear();assert h.latest()==null;
        // CRC-valid frame that repeats a logic level must still be rejected.
        byte[] invalid=f.clone();invalid[34]=2;CRC32 crc=new CRC32();crc.update(invalid,0,20);
        for(int n=0;n<21;n++){int p=20+(n/3)*20+5+(n%3)*5;crc.update(invalid,p,5);}
        long v=crc.getValue();for(int i=0;i<4;i++)invalid[168+i]=(byte)(v>>(8*i));
        h.connection(true,true);feed(h,invalid);assert h.latest()==null&&h.state().contains("电平不连续");
        // No-edge captures are real stable samples, not fabricated pulses.
        byte[] empty=new byte[40];System.arraycopy(f,0,empty,0,20);empty[2]=0;empty[3]=0;empty[14]=empty[15]=0;empty[20]=0x57;empty[21]=3;
        crc=new CRC32();crc.update(empty,0,20);v=crc.getValue();for(int i=0;i<4;i++)empty[28+i]=(byte)(v>>(8*i));
        h.clear();h.connection(true,true);feed(h,empty);assert h.latest()!=null&&h.latest().count()==0&&h.latest().complete();
        assert h.latest().measurements().contains("完整周期不足");
        System.out.println("PASS: C/Java pulse interoperability, CRC, missing/duplicate/invalid edges, stale captures and idle inputs");
    }
}
