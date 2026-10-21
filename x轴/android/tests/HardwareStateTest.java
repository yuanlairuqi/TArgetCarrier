import cn.robot.ble.*;
public class HardwareStateTest {
    static int cases;
    static void check(boolean value){if(!value)throw new AssertionError("case "+cases);cases++;}
    static Protocol.Status state(long esp,long stm,int motion,int requested,int speed,int flags){
        byte[] b=new byte[20];b[0]=3;b[1]=(byte)motion;b[2]=(byte)flags;b[3]=(byte)255;b[4]=(byte)255;
        b[5]=(byte)requested;b[6]=(byte)speed;b[19]=(byte)motion;
        for(int i=0;i<4;i++){b[8+i]=(byte)(esp>>>(8*i));b[12+i]=(byte)(stm>>>(8*i));}
        Protocol.Status s=Protocol.status(b);if(s==null)throw new AssertionError("fixture");return s;
    }
    public static void main(String[] args){
        HardwareState h=new HardwareState();CommandChannel c=new CommandChannel();
        check(!h.canMove(0));check(!h.confirmed(0));
        c.stop(0);CommandChannel.Packet stop=c.take(0);c.written();h.request(stop);
        Protocol.Status s=state(stop.sequence,stop.sequence,0,0,0,153);h.status(s,0);c.status(s);
        check(c.stopping());check(!h.confirmed(0)); // ACK without PEER_APPLIED must not clear STOP.
        s=state(stop.sequence,stop.sequence,0,0,0,185);h.status(s,1);c.status(s);
        check(!c.stopping());check(h.confirmed(1));check(h.canMove(1));
        check(c.movement(1,40,2));CommandChannel.Packet forward=c.take(2);c.written();h.request(forward);
        check(!h.confirmed(2));check(h.label(2,false).contains("等待正转"));
        h.status(state(forward.sequence,0,1,1,40,185),3);check(!h.confirmed(3));
        h.status(state(forward.sequence,forward.sequence,1,1,40,153),4);check(!h.confirmed(4));
        h.status(state(forward.sequence,forward.sequence,1,1,40,185),5);check(h.confirmed(5));
        check(h.label(5,false).contains("正向脉冲输出已确认"));
        check(c.movement(2,40,6));CommandChannel.Packet reverse=c.take(6);c.written();h.request(reverse);
        h.status(state(forward.sequence,forward.sequence,1,1,40,185),7);check(!h.confirmed(7));
        h.status(state(reverse.sequence,reverse.sequence,1,2,40,185),8);check(!h.confirmed(8));
        h.status(state(reverse.sequence,reverse.sequence,2,2,40,185),9);check(h.confirmed(9));
        h.status(state(999,999,2,2,40,185),10);check(!h.confirmed(10));
        h.status(state(reverse.sequence,reverse.sequence,2,2,40,185),11);
        check(!h.confirmed(1512));check(!h.canMove(1512));
        h.status(state(0,0,0,0,0,3),12);check(!h.canMove(12));check(!h.confirmed(12));
        h.status(state(0,0,0,0,0,129),13);check(!h.canMove(13));
        h.status(state(stop.sequence,stop.sequence,0,0,0,169),14);check(!h.canMove(14));
        h.reset();check(!h.confirmed(15));check(!h.canMove(15));
        h.status(state(reverse.sequence,reverse.sequence,2,2,40,185),16);check(!h.confirmed(16));
        System.out.println("PASS: "+cases+" hardware confirmation cases; no optimistic execution, demo/offline lock, stale/unknown ACK, reversal and reconnect");
    }
}
