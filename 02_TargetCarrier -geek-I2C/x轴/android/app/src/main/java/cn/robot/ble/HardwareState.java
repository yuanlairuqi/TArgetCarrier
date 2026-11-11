package cn.robot.ble;

import java.util.LinkedHashMap;

/** Correlates this BLE session's requests with STM32 software-output acknowledgements. */
public final class HardwareState {
    private static final long FRESH_MS=1500;
    private static final class Sent {
        final int motion,speed;
        Sent(CommandChannel.Packet packet){motion=packet.motion;speed=packet.bytes[2]&255;}
    }
    private final LinkedHashMap<Long,Sent> sent=new LinkedHashMap<>();
    private Protocol.Status latest;
    private long receivedAt,changedAtSequence;
    private int desired=-1,desiredSpeed;

    public void reset(){sent.clear();latest=null;receivedAt=0;desired=-1;desiredSpeed=0;changedAtSequence=0;}
    public void request(CommandChannel.Packet packet){
        Sent value=new Sent(packet);
        if(value.motion!=desired||value.speed!=desiredSpeed){changedAtSequence=packet.sequence;desired=value.motion;desiredSpeed=value.speed;}
        sent.put(packet.sequence,value);
        while(sent.size()>64)sent.remove(sent.keySet().iterator().next());
    }
    public void status(Protocol.Status value,long now){latest=value;receivedAt=now;}
    private boolean fresh(long now){return latest!=null&&now>=receivedAt&&now-receivedAt<=FRESH_MS;}
    private boolean hardware(long now){return fresh(now)&&latest.uart()&&!latest.demo()&&latest.stmOnline()&&(latest.flags&64)==0&&latest.error==0;}
    public boolean canMove(long now){return hardware(now)&&latest.ready()&&(latest.flags&16)!=0&&latest.motion!=5&&latest.runMode==0;}
    public boolean confirmed(long now){
        if(!hardware(now)||!latest.applied()||desired<0||latest.runMode!=0)return false;
        Sent packet=sent.get(latest.stmAck);
        long distance=(latest.stmAck-changedAtSequence)&0xffffffffL;
        return packet!=null&&distance<0x80000000L&&latest.ack==latest.stmAck&&
            packet.motion==desired&&packet.speed==desiredSpeed&&latest.commanded==desired&&
            latest.motion==desired&&latest.phase==desired&&(desired==0||latest.speed==desiredSpeed);
    }
    public String label(long now,boolean stopping){
        if(latest==null)return "等待硬件状态";
        if(!fresh(now))return "状态已过期 · 输出未知";
        if(!latest.uart()||latest.demo())return "演示模式 · 硬件控制锁定";
        if(!latest.stmOnline())return "STM32 离线 · 输出未知";
        if((latest.flags&64)!=0||latest.error!=0)return "设备拒绝／异常 · 请停止";
        if(latest.motion==5)return "STM32 本地动作 · 控制锁定";
        if(stopping)return "等待停止硬件确认";
        if((latest.flags&16)==0)return confirmed(now)&&desired==0?"停止已确认 · 连续运动未启用":"连续运动未启用";
        if(desired<0)return "等待停止同步";
        if(confirmed(now))return desired==0?"停止输出已确认":desired==1?"正向脉冲输出已确认":"反向脉冲输出已确认";
        return desired==0?"等待停止硬件确认":desired==1?"等待正转硬件确认":"等待反转硬件确认";
    }
    public String details(long now,boolean stopping){
        if(latest==null)return "连接后先发送 STOP，同步硬件状态";
        String request=desired<0?"尚未发送命令":Protocol.MOTION[desired]+(desired==0?"":" · 速度指令 "+desiredSpeed+"%");
        return "手机请求："+request+"\nESP32 接收序号 #"+latest.ack+
            "\nSTM32 回报确认 #"+latest.stmAck+" · "+(!stopping&&confirmed(now)?"匹配当前请求":"尚未匹配当前请求")+
            "\n输出确认不代表电机实测运动或到位";
    }
}
