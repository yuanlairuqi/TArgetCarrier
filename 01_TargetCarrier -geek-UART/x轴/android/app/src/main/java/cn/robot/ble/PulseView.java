package cn.robot.ble;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;
import java.util.Locale;

/** Draw only complete hardware snapshots. Gaps between captures are not joined. */
public final class PulseView extends View {
    private final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);
    private PulseHistory.Capture capture;
    private double startMs,spanMs=100;
    public PulseView(Context c){super(c);setBackgroundColor(0xff102532);}
    public void samples(PulseHistory.Capture s,double start,double span){capture=s;startMs=start;spanMs=span;invalidate();}
    private float dp(float n){return n*getResources().getDisplayMetrics().density;}
    private void text(Canvas c,String s,float x,float y,int color,float size){p.setColor(color);p.setTextSize(dp(size));p.setStrokeWidth(1);c.drawText(s,x,y,p);}
    @Override protected void onDraw(Canvas c){
        super.onDraw(c);float left=dp(48),right=getWidth()-dp(14),top=dp(40),bottom=getHeight()-dp(32);
        if(right<=left||bottom<=top)return;
        for(int i=0;i<=5;i++){
            float x=left+(right-left)*i/5f;p.setColor(0xff29414e);p.setStrokeWidth(dp(1));c.drawLine(x,top,x,bottom,p);
            text(c,String.format(Locale.ROOT,"%.1f",startMs+spanMs*i/5),x-dp(10),getHeight()-dp(12),0xffa7bdc9,10);
        }
        text(c,"时间 / ms",left,dp(22),0xffa7bdc9,12);
        if(capture==null||!capture.complete()){
            text(c,capture==null?"等待真实采样数据":"本段不可用，不补画方波",left,top+dp(45),Color.WHITE,14);return;
        }
        double from=startMs*capture.hz/1000,to=(startMs+spanMs)*capture.hz/1000;
        c.save();c.clipRect(left,top,right,bottom);
        for(int channel=0;channel<2;channel++){
            float band=(bottom-top)/2,high=top+band*channel+band*.22f,low=top+band*channel+band*.72f;
            int color=channel==0?0xff43d9c0:0xffffca68,level=(capture.initial>>channel)&1;
            double previous=from;
            for(int i=0;i<capture.count();i++)if(capture.channel(i)==channel){
                double t=capture.tick(i);
                if(t<from){level=capture.level(i);continue;}if(t>to)break;
                float x1=left+(float)((previous-from)/(to-from))*(right-left);
                float x2=left+(float)((t-from)/(to-from))*(right-left);
                p.setColor(color);p.setStrokeWidth(dp(2));float y=level==1?high:low;
                c.drawLine(x1,y,x2,y,p);level=capture.level(i);c.drawLine(x2,y,x2,level==1?high:low,p);previous=t;
            }
            p.setColor(color);p.setStrokeWidth(dp(2));float x=left+(float)((previous-from)/(to-from))*(right-left);
            c.drawLine(x,level==1?high:low,right,level==1?high:low,p);
        }
        c.restore();
        text(c,"PUL",dp(5),top+(bottom-top)*.16f,0xff43d9c0,12);
        text(c,"DIR",dp(5),top+(bottom-top)*.66f,0xffffca68,12);
        text(c,"1",dp(30),top+(bottom-top)*.11f,0xffa7bdc9,10);text(c,"0",dp(30),top+(bottom-top)*.36f,0xffa7bdc9,10);
        text(c,"1",dp(30),top+(bottom-top)*.61f,0xffa7bdc9,10);text(c,"0",dp(30),top+(bottom-top)*.86f,0xffa7bdc9,10);
    }
}
