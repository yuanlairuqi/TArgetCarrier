package cn.robot.ble;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/** Bounded history of actual BLE notifications. No samples are synthesized. */
public final class ReceiveHistory {
    public static final String[] METRICS = {
        "回报指令速度 (%)", "有效电压 (V)", "蓝牙 RSSI (dBm)",
        "STM32 在线 (0/1)", "STM32 输出应用标志 (0/1)", "STM32 动作代码"
    };
    public static final long GAP_MS = 1500;

    public static final class Frame {
        public final long number, elapsedMs, wallMs;
        public final int segment;
        public final Protocol.Status status;
        public final double rssi;
        private final byte[] payload;

        Frame(long number, long elapsedMs, long wallMs, int segment,
              byte[] bytes, Protocol.Status status, double rssi) {
            this.number=number; this.elapsedMs=elapsedMs; this.wallMs=wallMs;
            this.segment=segment; this.payload=bytes==null?new byte[0]:bytes.clone();
            this.status=status; this.rssi=rssi;
        }
        public int length() { return payload.length; }
        public String hex() {
            StringBuilder b=new StringBuilder(payload.length*3);
            final char[] digits="0123456789ABCDEF".toCharArray();
            for(byte value:payload) {
                if(b.length()>0)b.append(' ');
                int n=value&255; b.append(digits[n>>>4]).append(digits[n&15]);
            }
            return b.toString();
        }
        public double value(int metric) {
            if(status==null)return Double.NaN;
            switch(metric) {
                case 0:return status.speed;
                case 1:return status.batteryValid()?status.batteryMv/1000.0:Double.NaN;
                case 2:return rssi;
                case 3:return status.uart()?(status.stmOnline()?1:0):Double.NaN;
                case 4:return status.uart()&&status.stmOnline()&&!status.demo()?(status.applied()?1:0):Double.NaN;
                case 5:return status.uart()&&status.stmOnline()&&!status.demo()?status.motion:Double.NaN;
                default:return Double.NaN;
            }
        }
    }

    public static final class Snapshot {
        public final List<Frame> frames;
        public final String log, link;
        public final boolean connected;
        public final long received, invalid, now, revision;
        public final int recentFrames;
        public final Frame latest;
        Snapshot(List<Frame> frames,String log,String link,boolean connected,long received,
                 long invalid,long now,long revision,int recentFrames,Frame latest) {
            this.frames=Collections.unmodifiableList(frames); this.log=log; this.link=link;
            this.connected=connected; this.received=received; this.invalid=invalid;
            this.now=now; this.revision=revision; this.recentFrames=recentFrames; this.latest=latest;
        }
    }

    private final int frameLimit,logLimit;
    private final ArrayDeque<Frame> frames=new ArrayDeque<>();
    private final ArrayDeque<String> lines=new ArrayDeque<>();
    private final SimpleDateFormat clock=new SimpleDateFormat("HH:mm:ss.SSS",Locale.ROOT);
    private long received,invalid,revision;
    private int segment;
    private Frame latest;
    private boolean connected;
    private String link="未连接";

    public ReceiveHistory(int frameLimit,int logLimit) {
        if(frameLimit<1||logLimit<1)throw new IllegalArgumentException("Positive history limits required");
        this.frameLimit=frameLimit;this.logLimit=logLimit;
    }
    public void connection(boolean connected,String description,long wallMs) {
        if(this.connected!=connected)++segment;
        this.connected=connected;link=description;
        event(description,wallMs);
    }
    public void event(String text,long wallMs) {
        line(clock.format(new Date(wallMs))+"  · "+text);
    }
    private void line(String text) {
        ++revision;
        while(lines.size()>=logLimit)lines.removeFirst();
        lines.addLast(text);
    }
    public Protocol.Status receive(long elapsedMs,long wallMs,byte[] bytes,double rssi) {
        Protocol.Status status=Protocol.status(bytes);
        Frame frame=new Frame(++received,elapsedMs,wallMs,segment,bytes,status,rssi);
        while(frames.size()>=frameLimit)frames.removeFirst();
        frames.addLast(frame);
        if(status==null)++invalid;else latest=frame;
        String summary=status==null?"格式无效":("指令 "+status.speed+"% · STM "+
            (status.stmOnline()?"在线":"离线")+" · ACK "+status.ack+" / "+status.stmAck);
        line(clock.format(new Date(wallMs))+"  RX #"+frame.number+"  "+frame.length()+" B  "+summary+
             "\n"+frame.hex());
        return status;
    }
    /** Clear only the monitor history, never the connection or command channel. */
    public void clear() {
        frames.clear();lines.clear();received=invalid=0;latest=null;++revision;
    }
    public Snapshot snapshot(long now) {
        int recent=0;
        for(Frame frame:frames)if(now>=frame.elapsedMs&&now-frame.elapsedMs<=1000)++recent;
        StringBuilder log=new StringBuilder();
        for(String text:lines){if(log.length()>0)log.append("\n\n");log.append(text);}
        return new Snapshot(new ArrayList<>(frames),log.toString(),link,connected,
            received,invalid,now,revision,recent,latest);
    }
}
