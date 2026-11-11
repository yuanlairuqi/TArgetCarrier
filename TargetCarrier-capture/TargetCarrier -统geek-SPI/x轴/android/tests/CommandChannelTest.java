import cn.robot.ble.*;
public class CommandChannelTest {
    static Protocol.Status status(long esp,long stm,int motion,int flags) {
        byte[] b=new byte[20];b[0]=3;b[1]=(byte)motion;b[2]=(byte)flags;b[3]=(byte)255;b[4]=(byte)255;b[5]=(byte)motion;
        for(int i=0;i<4;i++){b[8+i]=(byte)(esp>>>(8*i));b[12+i]=(byte)(stm>>>(8*i));}return Protocol.status(b);
    }
    static CommandChannel synced(boolean uart) {
        CommandChannel c=new CommandChannel();c.reset();c.stop(0);CommandChannel.Packet p=c.take(0);c.written();
        c.status(status(p.sequence,p.sequence,0,uart?185:3));assert c.synchronizedOnce()&&!c.stopping();return c;
    }
    public static void main(String[] args) {
        CommandChannel c=new CommandChannel();c.reset();assert !c.movement(1,40,0);
        c.stop(0);c.stop(1);CommandChannel.Packet p=c.take(2);assert p!=null&&p.motion==0; // repeated release never erases pending STOP
        c.written();c.status(status(p.sequence,0,0,185));assert !c.synchronizedOnce(); // ESP alone can't stand in for STM32
        c.status(status(999,999,0,185));assert !c.synchronizedOnce();
        c.status(status(p.sequence,p.sequence,1,153));assert c.stopping(); // mismatched state cannot acknowledge stop
        assert c.acknowledgementExpired(1002)==0;
        c.status(status(p.sequence,p.sequence,0,185));assert c.synchronizedOnce();
        assert c.movement(1,40,1100);p=c.take(1100);assert p.motion==1;
        assert c.movement(2,40,1110);assert c.take(1110)==null; // serialized GATT
        c.stop(1111);c.stop(1112);assert !c.movement(2,40,1113);
        c.written();CommandChannel.Packet stop=c.take(1114);assert stop.motion==0; // STOP supersedes queued movement
        c.written();c.status(status(stop.sequence,stop.sequence,0,185));assert !c.stopping();assert c.take(1115)==null;
        c=synced(false);c.movement(1,40,100);p=c.take(100);c.written();
        c.status(status(1,0,0,3));assert c.acknowledgementExpired(1099)==-1;assert c.acknowledgementExpired(1100)==1;
        assert c.acknowledgementExpired(1101)==-1; // duplicate ACK doesn't reset deadline, one error report
        c.stop(1102);assert c.take(1102).motion==0;
        c=synced(false);c.movement(1,40,0);assert c.take(301)==null; // stale unsent movement is discarded
        c.movement(1,40,400);c.take(400);assert !c.writingExpired(1899);assert c.writingExpired(1900);
        c.reset();assert !c.synchronizedOnce()&&c.idle(); // reconnect never resumes previous hold
        c=synced(false);c.parameters(1,1200,2500);assert c.movement(1,60,0);p=c.take(0);
        assert p.bytes.length==16&&p.bytes[3]==1&&p.bytes[8]==(byte)0xb0&&p.bytes[12]==(byte)0xc4;
        // UART movement must be both applied and consistent with the sent request.
        for(int kind=0;kind<7;kind++) {
            c=synced(true);c.movement(1,40,100);p=c.take(100);c.written();
            byte[] b=new byte[20];b[0]=3;b[1]=1;b[2]=(byte)185;b[3]=(byte)255;b[4]=(byte)255;b[5]=1;b[6]=40;b[19]=1;
            for(int i=0;i<4;i++){b[8+i]=(byte)(p.sequence>>>(8*i));b[12+i]=(byte)(p.sequence>>>(8*i));}
            if(kind==0)b[2]=(byte)153; // no APPLIED
            if(kind==1)b[6]=41; // wrong speed
            if(kind==2){b[1]=2;b[5]=2;} // wrong direction
            if(kind==3)b[8]++; // ESP and STM ACK disagree
            if(kind==4)b[2]=(byte)249; // fault
            if(kind==5)b[19]=2; // wrong output phase
            if(kind==6)b[18]=1; // wrong run mode
            c.status(Protocol.status(b));assert c.acknowledgementExpired(1100)==1: "accepted invalid movement status "+kind;
        }
        c=synced(true);c.movement(1,40,100);p=c.take(100);c.written();
        byte[] good=new byte[20];good[0]=3;good[1]=1;good[2]=(byte)185;good[3]=(byte)255;good[4]=(byte)255;good[5]=1;good[6]=40;good[19]=1;
        for(int i=0;i<4;i++){good[8+i]=(byte)(p.sequence>>>(8*i));good[12+i]=(byte)(p.sequence>>>(8*i));}
        c.status(Protocol.status(good));assert c.acknowledgementExpired(1100)==-1;
        System.out.println("PASS: production Android queue, two-level STOP confirmation, stale/unknown ACK, STOP priority, serialized GATT, bounded old movement, ACK/write deadlines, reconnect reset");
    }
}
