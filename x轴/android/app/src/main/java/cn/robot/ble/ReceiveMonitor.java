package cn.robot.ble;

import android.app.Activity;
import android.app.Dialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.util.Locale;

/** A separate Dialog window owned by MainActivity; opening it does not pause BLE. */
public final class ReceiveMonitor {
    private static final int INK=0xff172b36,MUTED=0xff647680,ACCENT=0xff087f8c;
    private final Activity activity;
    private final ReceiveHistory history;
    private final Runnable stopAction;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private Dialog dialog;
    private TextView status,summary,raw,chartNote;
    private Button pause,dataTab,chartTab;
    private ScrollView logScroll;
    private View chartPanel;
    private WaveformView waveform;
    private boolean paused,chart;
    private int metric;
    private long span=30000,lastRenderedRevision=-1;
    private ReceiveHistory.Snapshot frozen;
    private final Runnable tick=new Runnable(){public void run(){
        if(dialog==null||!dialog.isShowing())return;
        render();handler.postDelayed(this,200);
    }};

    public ReceiveMonitor(Activity activity,ReceiveHistory history,Runnable stopAction) {
        this.activity=activity;this.history=history;this.stopAction=stopAction;
    }
    private int dp(int n){return Math.round(n*activity.getResources().getDisplayMetrics().density);}
    private GradientDrawable background(int color,int radius) {
        GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(radius));return d;
    }
    private TextView text(String value,int size,int color,boolean bold) {
        TextView t=new TextView(activity);t.setText(value);t.setTextSize(size);t.setTextColor(color);
        if(bold)t.setTypeface(null,Typeface.BOLD);
        t.setPadding(0,dp(4),0,dp(4));return t;
    }
    private Button button(String title,int color) {
        Button b=new Button(activity);b.setText(title);b.setTextSize(13);b.setTextColor(Color.WHITE);
        b.setAllCaps(false);b.setMinWidth(0);b.setMinimumWidth(0);b.setMinHeight(dp(44));b.setMinimumHeight(dp(44));
        b.setPadding(dp(5),0,dp(5),0);b.setBackground(background(color,10));return b;
    }
    private void cell(LinearLayout row,View v,float weight) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(44),weight);
        p.setMargins(dp(3),dp(3),dp(3),dp(3));row.addView(v,p);
    }
    public void show() {
        if(dialog!=null&&dialog.isShowing())return;
        paused=false;frozen=null;lastRenderedRevision=-1;
        dialog=new Dialog(activity);dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout root=new LinearLayout(activity);root.setOrientation(1);
        root.setPadding(dp(14),dp(10),dp(14),dp(10));root.setBackgroundColor(0xfff3f5f7);
        root.setOnApplyWindowInsetsListener((v,insets)->{
            if(Build.VERSION.SDK_INT>=30) {
                android.graphics.Insets x=insets.getInsets(WindowInsets.Type.systemBars());
                v.setPadding(dp(14)+x.left,dp(10)+x.top,dp(14)+x.right,dp(10)+x.bottom);
            } else v.setPadding(dp(14)+insets.getSystemWindowInsetLeft(),dp(10)+insets.getSystemWindowInsetTop(),
                dp(14)+insets.getSystemWindowInsetRight(),dp(10)+insets.getSystemWindowInsetBottom());
            return insets;
        });
        LinearLayout header=new LinearLayout(activity);header.setGravity(android.view.Gravity.CENTER_VERTICAL);
        header.addView(text("接收监视",23,INK,true),new LinearLayout.LayoutParams(0,-2,1));
        Button stop=button("停止",0xffd34b46);header.addView(stop,new LinearLayout.LayoutParams(dp(66),dp(44)));
        stop.setOnClickListener(v->stopAction.run());
        Button close=button("关闭",MUTED);LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(dp(66),dp(44));
        cp.leftMargin=dp(8);header.addView(close,cp);close.setOnClickListener(v->dismiss());root.addView(header);
        status=text("",12,ACCENT,false);root.addView(status);
        TextView note=text("显示状态回传及参数曲线；真实 PUL/DIR 请打开主页面的独立采集窗口。",12,MUTED,false);
        root.addView(note);
        summary=text("等待有效回传帧",12,INK,false);summary.setTypeface(Typeface.MONOSPACE);
        summary.setPadding(dp(10),dp(8),dp(10),dp(8));summary.setBackground(background(Color.WHITE,12));
        root.addView(summary);

        LinearLayout toolbar=new LinearLayout(activity);
        dataTab=button("原始数据",ACCENT);chartTab=button("参数曲线",MUTED);
        pause=button("暂停显示",MUTED);Button clear=button("清空",MUTED);
        cell(toolbar,dataTab,1.2f);cell(toolbar,chartTab,1.2f);cell(toolbar,pause,1.2f);cell(toolbar,clear,.8f);
        root.addView(toolbar);
        FrameLayout content=new FrameLayout(activity);
        LinearLayout.LayoutParams contentParams=new LinearLayout.LayoutParams(-1,0,1);contentParams.topMargin=dp(5);
        root.addView(content,contentParams);
        logScroll=new ScrollView(activity);logScroll.setBackground(background(Color.WHITE,12));
        raw=text("",11,INK,false);raw.setTypeface(Typeface.MONOSPACE);raw.setTextIsSelectable(true);
        raw.setPadding(dp(10),dp(8),dp(10),dp(12));logScroll.addView(raw);
        content.addView(logScroll,new FrameLayout.LayoutParams(-1,-1));

        LinearLayout plots=new LinearLayout(activity);plots.setOrientation(1);chartPanel=plots;
        LinearLayout choices=new LinearLayout(activity);
        Spinner metrics=new Spinner(activity);
        metrics.setAdapter(new ArrayAdapter<String>(activity,android.R.layout.simple_spinner_dropdown_item,ReceiveHistory.METRICS));
        choices.addView(metrics,new LinearLayout.LayoutParams(0,dp(48),2.3f));
        Spinner windows=new Spinner(activity);
        windows.setAdapter(new ArrayAdapter<String>(activity,android.R.layout.simple_spinner_dropdown_item,new String[]{"10 秒","30 秒","60 秒"}));
        windows.setSelection(span==10000?0:span==60000?2:1);
        choices.addView(windows,new LinearLayout.LayoutParams(0,dp(48),1));plots.addView(choices);
        waveform=new WaveformView(activity);plots.addView(waveform,new LinearLayout.LayoutParams(-1,0,1));
        chartNote=text("",11,MUTED,false);plots.addView(chartNote);
        content.addView(plots,new FrameLayout.LayoutParams(-1,-1));
        metrics.setSelection(metric);
        metrics.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener(){
            public void onItemSelected(android.widget.AdapterView<?> p,View v,int n,long id){metric=n;render();}
            public void onNothingSelected(android.widget.AdapterView<?> p){}
        });
        windows.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener(){
            public void onItemSelected(android.widget.AdapterView<?> p,View v,int n,long id){span=new long[]{10000,30000,60000}[n];render();}
            public void onNothingSelected(android.widget.AdapterView<?> p){}
        });
        Button copy=button("复制当前接收记录",ACCENT);LinearLayout.LayoutParams copyParams=new LinearLayout.LayoutParams(-1,dp(44));
        copyParams.topMargin=dp(7);root.addView(copy,copyParams);
        root.addView(text("暂停仅冻结显示，接收继续；断线、无效值和超时处不补画数据。",11,MUTED,false));
        dataTab.setOnClickListener(v->{chart=false;tabs();render();});
        chartTab.setOnClickListener(v->{chart=true;tabs();render();});
        pause.setOnClickListener(v->{
            paused=!paused;frozen=paused?history.snapshot(SystemClock.elapsedRealtime()):null;
            pause.setText(paused?"继续显示":"暂停显示");render();
        });
        clear.setOnClickListener(v->{
            history.clear();frozen=paused?history.snapshot(SystemClock.elapsedRealtime()):null;lastRenderedRevision=-1;render();
        });
        copy.setOnClickListener(v->{
            ReceiveHistory.Snapshot s=paused?frozen:history.snapshot(SystemClock.elapsedRealtime());
            ClipboardManager clipboard=(ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if(clipboard!=null) {
                clipboard.setPrimaryClip(ClipData.newPlainText("ESP32接收记录",s.log.length()==0?"暂无接收记录":s.log));
                Toast.makeText(activity,"已复制接收记录",Toast.LENGTH_SHORT).show();
            }
        });
        dialog.setContentView(root);dialog.setOnDismissListener(d->{handler.removeCallbacks(tick);dialog=null;frozen=null;});
        dialog.show();Window window=dialog.getWindow();
        if(window!=null) {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.setLayout(WindowManager.LayoutParams.MATCH_PARENT,WindowManager.LayoutParams.MATCH_PARENT);
        }
        root.requestApplyInsets();tabs();handler.post(tick);
    }
    private void tabs() {
        logScroll.setVisibility(chart?View.GONE:View.VISIBLE);chartPanel.setVisibility(chart?View.VISIBLE:View.GONE);
        summary.setVisibility(chart?View.GONE:View.VISIBLE);
        dataTab.setBackground(background(chart?MUTED:ACCENT,10));chartTab.setBackground(background(chart?ACCENT:MUTED,10));
    }
    private void render() {
        if(dialog==null||!dialog.isShowing()||summary==null||waveform==null)return;
        long now=SystemClock.elapsedRealtime();ReceiveHistory.Snapshot live=history.snapshot(now);
        ReceiveHistory.Snapshot s=paused?frozen:live;if(s==null)return;
        String age=s.latest==null?"无有效帧":Math.max(0,now-s.latest.elapsedMs)+" ms 前";
        status.setText((paused?"显示已暂停 · ":"")+live.link+"\n接收 "+s.received+" 帧 · 无效 "+s.invalid+
            " · 最近1秒 "+s.recentFrames+" 帧 · 最近有效帧 "+age);
        if(s.latest==null)summary.setText("等待有效回传帧\n收到数据后显示指令、确认序号、电压、状态和错误码。");
        else {
            Protocol.Status value=s.latest.status;
            String motion=!value.uart()?"BLE演示："+Protocol.MOTION[value.motion]:
                !value.stmOnline()?"未知（STM32离线）":
                (value.demo()?"通信演示：":"STM状态：")+Protocol.MOTION[value.motion];
            String voltage=value.batteryValid()?String.format(Locale.ROOT,"%.3f V",value.batteryMv/1000.0):"未接入 / 无效";
            summary.setText("最后有效帧 #"+s.latest.number+" · "+s.latest.length()+" B"+
                "\n指令 "+Protocol.MOTION[value.commanded]+"  "+value.speed+"% · 回报 "+motion+
                "\n电压 "+voltage+" · UART "+(value.uart()?"已配置":"未配置")+
                "\nESP确认 #"+value.ack+" · STM确认 "+(value.stmOnline()?"#"+value.stmAck:"—")+
                "\n标志 "+String.format(Locale.ROOT,"0x%02X",value.flags)+" · 模式 "+value.runMode+" · 相位 "+value.phase+
                "\n错误 "+value.error+"："+Protocol.ERRORS[value.error]+" · 设备秒计数 "+value.uptime);
        }
        if(!chart&&lastRenderedRevision!=s.revision) {
            raw.setText(s.log.length()==0?"等待接收…":s.log);lastRenderedRevision=s.revision;
            if(!paused)logScroll.post(()->logScroll.fullScroll(View.FOCUS_DOWN));
        }
        waveform.samples(s,metric,span);
        chartNote.setText(metric==0?"速度为回报的指令百分比，不是测得转速。":
            metric==5?"动作代码：0停止，1正转，2反转，5本地动作。":
            metric==1?"电压有效标志未置位时留空，不用0 V代替。":
            metric==2?"RSSI为手机最近一次链路读取值，超过7秒不画。":"只显示真实回传样本；时间轴使用手机接收时间。");
    }
    public void dismiss() {
        handler.removeCallbacks(tick);
        if(dialog!=null)dialog.dismiss();
    }
}
