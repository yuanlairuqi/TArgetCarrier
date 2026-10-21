package cn.robot.ble;

import java.util.Locale;
import java.util.UUID;
import java.util.zip.CRC32;

/** Strict assembly of hardware-edge snapshots. Incomplete frames are never drawn. */
public final class PulseHistory {
    public static final UUID CHARACTERISTIC=UUID.fromString("6e400004-b5a3-f393-e0a9-e50e24dcca9e");
    public static final int MAX_EDGES=512;
    public static final class Capture {
        public final int id,flags,initial,pulGpio,dirGpio;
        public final long hz,duration,receivedAt;
        private final long[] ticks;
        private final byte[] tags;
        Capture(int id,int flags,int initial,int pul,int dir,long hz,long duration,long at,long[] ticks,byte[] tags){
            this.id=id;this.flags=flags;this.initial=initial;pulGpio=pul;dirGpio=dir;
            this.hz=hz;this.duration=duration;receivedAt=at;this.ticks=ticks.clone();this.tags=tags.clone();
        }
        public int count(){return ticks.length;}
        public long tick(int n){return ticks[n];}
        public int channel(int n){return tags[n]&1;}
        public int level(int n){return (tags[n]>>1)&1;}
        public boolean available(){return (flags&1)!=0;}
        public boolean complete(){return available()&&(flags&6)==0;}
        public double durationMs(){return hz==0?0:duration*1000.0/hz;}
        public String measurements(){
            if(!complete())return "采样溢出或边沿不连续，本段不绘图、不计算测量值。";
            StringBuilder out=new StringBuilder();
            for(int ch=0;ch<2;ch++){
                long firstRise=-1,lastRise=-1,previous=-1;int rises=0,lastLevel=-1,highN=0,lowN=0;
                long highMin=Long.MAX_VALUE,highMax=0,lowMin=Long.MAX_VALUE,lowMax=0;
                for(int i=0;i<count();i++)if(channel(i)==ch){
                    long t=tick(i);int level=level(i);
                    if(level==1){if(firstRise<0)firstRise=t;lastRise=t;rises++;}
                    if(previous>=0){long d=t-previous;
                        if(lastLevel==1){highMin=Math.min(highMin,d);highMax=Math.max(highMax,d);highN++;}
                        else{lowMin=Math.min(lowMin,d);lowMax=Math.max(lowMax,d);lowN++;}}
                    previous=t;lastLevel=level;
                }
                if(ch>0)out.append('\n');out.append(ch==0?"PUL":"DIR");
                if(rises>=2&&lastRise>firstRise)out.append(String.format(Locale.ROOT,"  %.3f Hz · 周期 %.3f ms",
                    (rises-1)*hz/(double)(lastRise-firstRise),(lastRise-firstRise)*1000.0/hz/(rises-1)));
                else out.append("  完整周期不足");
                if(highN>0)out.append(String.format(Locale.ROOT,"\n高 %.3f–%.3f ms",highMin*1000.0/hz,highMax*1000.0/hz));
                if(lowN>0)out.append(String.format(Locale.ROOT,"  低 %.3f–%.3f ms",lowMin*1000.0/hz,lowMax*1000.0/hz));
            }
            return out.toString();
        }
        public String csv(){
            StringBuilder s=new StringBuilder("# Hardware logic capture; initial PUL=").append(initial&1)
                .append(" DIR=").append((initial>>1)&1).append("; flags=").append(flags)
                .append("; clock_hz=").append(hz).append("; window_ticks=").append(duration)
                .append("\nchannel,time_ticks,time_ms,level\n");
            for(int i=0;i<count();i++)s.append(channel(i)==0?"PUL":"DIR").append(',').append(tick(i)).append(',')
                .append(String.format(Locale.ROOT,"%.6f",tick(i)*1000.0/hz)).append(',').append(level(i)).append('\n');
            return s.toString();
        }
    }
    private byte[] header;
    private long[] ticks;
    private byte[] tags;
    private int id,expected,received,sequence;
    private CRC32 crc;
    private Capture latest;
    private String state="连接支持采集的新固件后显示真实波形";
    private boolean connected,supported;
    private long errors;
    private static int u16(byte[] b,int p){return (b[p]&255)|((b[p+1]&255)<<8);}
    private static long u32(byte[] b,int p){return (b[p]&255L)|((b[p+1]&255L)<<8)|((b[p+2]&255L)<<16)|((b[p+3]&255L)<<24);}
    public synchronized void connection(boolean connected,boolean supported){
        this.connected=connected;this.supported=supported;header=null;
        state=!connected?"已断线；保留的是历史采样":supported?"等待硬件采集数据":"当前 S3 固件不支持真实脉冲采集，请更新配套固件";
    }
    private void invalid(String reason){header=null;errors++;state=reason+"；等待下一段完整采样";}
    public synchronized void receive(byte[] b,long now){
        if(!connected||!supported)return;
        if(b==null||b.length!=20||b[0]!=0x57){invalid("波形帧格式错误");return;}
        int kind=b[1]&255,packetId=u16(b,2);
        if(kind==1){
            if(header!=null)errors++;
            int count=u16(b,14),flags=b[4]&255;long hz=u32(b,6),duration=u32(b,10);
            if(b[18]!=1||b[19]!=0||count>MAX_EDGES||(flags&~7)!=0||(b[5]&~3)!=0||
                ((flags&1)!=0&&(hz==0||duration==0||duration>hz||b[16]<0||b[17]<0||b[16]==b[17]))){invalid("采样头无效");return;}
            header=b.clone();id=packetId;expected=count;received=sequence=0;
            ticks=new long[count];tags=new byte[count];crc=new CRC32();crc.update(header);
            state="接收采样 #"+id+" · 0/"+expected+" 边沿";return;
        }
        if(header==null||packetId!=id){invalid("采样序号不连续");return;}
        if(kind==2){
            if((b[4]&255)!=sequence||received>=expected){invalid("波形数据缺包或重复");return;}
            int n=Math.min(3,expected-received);
            for(int k=0;k<n;k++){
                int p=5+k*5;long t=u32(b,p);int tag=b[p+4]&255;
                if(t>=u32(header,10)||tag>3||(received>0&&t<ticks[received-1])){invalid("边沿时间或通道无效");return;}
                ticks[received]=t;tags[received]=(byte)tag;received++;crc.update(b,p,5);
            }
            sequence++;state="接收采样 #"+id+" · "+received+"/"+expected+" 边沿";return;
        }
        if(kind!=3||received!=expected||u16(b,4)!=sequence||u16(b,6)!=expected||u32(b,8)!=crc.getValue()){
            invalid("采样不完整或校验失败");return;
        }
        // Validate actual level transitions independently of the firmware's flags.
        int levels=header[5]&3;
        for(int i=0;i<expected;i++){
            int ch=tags[i]&1,value=(tags[i]>>1)&1;
            if(((levels>>ch)&1)==value){invalid("边沿电平不连续");return;}
            levels=(levels&~(1<<ch))|(value<<ch);
        }
        latest=new Capture(id,header[4]&255,header[5]&3,header[16]&255,header[17]&255,
            u32(header,6),u32(header,10),now,ticks,tags);header=null;
        state=!latest.available()?"S3 采集未启用或初始化失败":!latest.complete()?"检测到采样丢失：本段留空":
            "已收到完整硬件采样 #"+id+" · "+expected+" 边沿";
    }
    public synchronized Capture latest(){return latest;}
    public synchronized String state(){return state+" · 接收异常次数 "+errors;}
    public synchronized void clear(){header=null;latest=null;errors=0;state="已清空，等待下一段采样";}
}
