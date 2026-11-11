package cn.robot.ble;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Time plots of received state values, not an oscilloscope or generated PWM. */
public final class WaveformView extends View {
    private final Paint grid=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint trace=new Paint(Paint.ANTI_ALIAS_FLAG);
    private List<ReceiveHistory.Frame> frames=Collections.emptyList();
    private int metric;
    private long now,span=30000;
    private final float density;
    public WaveformView(Context context) {
        super(context);density=getResources().getDisplayMetrics().density;
        grid.setColor(0xffdde5eb);grid.setStrokeWidth(density);
        label.setColor(0xff647680);label.setTextSize(11*density);
        trace.setColor(0xff087f8c);trace.setStrokeWidth(2*density);trace.setStyle(Paint.Style.STROKE);
        setBackgroundColor(Color.WHITE);setMinimumHeight((int)(180*density));
        setContentDescription("接收参数随时间变化的曲线；不表示实际PUL电气波形");
    }
    public void samples(ReceiveHistory.Snapshot snapshot,int metric,long span) {
        this.frames=snapshot.frames;this.now=snapshot.now;this.metric=metric;this.span=span;invalidate();
    }
    private float d(float n){return n*density;}
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float left=d(51),right=getWidth()-d(16),top=d(18),bottom=getHeight()-d(34);
        if(right<=left||bottom<=top)return;
        long start=now-span;
        double min=Double.POSITIVE_INFINITY,max=Double.NEGATIVE_INFINITY;
        int count=0;
        for(ReceiveHistory.Frame f:frames) {
            double value=f.value(metric);
            if(f.elapsedMs>=start&&f.elapsedMs<=now&&!Double.isNaN(value)) {
                min=Math.min(min,value);max=Math.max(max,value);++count;
            }
        }
        if(metric==0){min=0;max=100;}
        else if(metric==2){min=count==0?-100:Math.min(-100,Math.floor(min/10)*10);max=count==0?-20:Math.max(-20,Math.ceil(max/10)*10);}
        else if(metric==3||metric==4){min=0;max=1;}
        else if(metric==5){min=0;max=5;}
        else if(count==0){min=0;max=1;}
        else {double pad=Math.max(.1,(max-min)*.15);min=Math.max(0,min-pad);max+=pad;}
        if(max<=min)max=min+1;
        label.setTextAlign(Paint.Align.RIGHT);
        for(int i=0;i<=4;i++) {
            float y=bottom-(bottom-top)*i/4;
            canvas.drawLine(left,y,right,y,grid);
            double v=min+(max-min)*i/4;
            String text=(metric==1)?String.format(Locale.ROOT,"%.2f",v):String.format(Locale.ROOT,"%.1f",v);
            canvas.drawText(text,left-d(7),y+d(4),label);
        }
        for(int i=0;i<=3;i++) {
            float x=left+(right-left)*i/3;
            canvas.drawLine(x,top,x,bottom,grid);
            label.setTextAlign(i==0?Paint.Align.LEFT:i==3?Paint.Align.RIGHT:Paint.Align.CENTER);
            String text=i==3?"0 s":String.format(Locale.ROOT,"−%.0f s",span/1000.0*(3-i)/3);
            canvas.drawText(text,x,bottom+d(21),label);
        }
        if(count==0) {
            label.setTextAlign(Paint.Align.CENTER);label.setTextSize(d(14));
            canvas.drawText("暂无有效数据",(left+right)/2,(top+bottom)/2,label);
            label.setTextSize(d(11));return;
        }
        canvas.save();canvas.clipRect(left,top,right,bottom);
        ReceiveHistory.Frame previous=null;float px=0,py=0;
        for(ReceiveHistory.Frame f:frames) {
            if(f.elapsedMs<start||f.elapsedMs>now){previous=null;continue;}
            double value=f.value(metric);
            if(Double.isNaN(value)){previous=null;continue;}
            float x=left+(right-left)*(f.elapsedMs-start)/(float)span;
            float y=bottom-(float)((value-min)/(max-min))*(bottom-top);
            if(previous!=null&&previous.segment==f.segment&&f.elapsedMs>=previous.elapsedMs&&
               f.elapsedMs-previous.elapsedMs<=ReceiveHistory.GAP_MS) {
                if(metric>=3){canvas.drawLine(px,py,x,py,trace);canvas.drawLine(x,py,x,y,trace);}
                else canvas.drawLine(px,py,x,y,trace);
            }
            canvas.drawCircle(x,y,d(1.5f),trace);
            previous=f;px=x;py=y;
        }
        canvas.restore();
    }
}
