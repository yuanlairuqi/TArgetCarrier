package cn.robot.ble;

import android.app.Activity;
import android.app.Dialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.*;
import java.util.Locale;

/** Same-activity window: opening it keeps the BLE control lifecycle intact. */
public final class PulseMonitor {
    private final Activity activity;
    private final PulseHistory history;
    private final Runnable stop;
    private final Handler ui=new Handler(Looper.getMainLooper());
    private Dialog dialog;
    private PulseView plot;
    private TextView status,measurements;
    private boolean frozen;
    private PulseHistory.Capture saved;
    private double span=100,offset;
    public PulseMonitor(Activity a,PulseHistory h,Runnable stop){activity=a;history=h;this.stop=stop;}
    private int dp(int n){return Math.round(n*activity.getResources().getDisplayMetrics().density);}
    private TextView text(String value,int size){TextView t=new TextView(activity);t.setText(value);t.setTextSize(size);t.setTextColor(0xff172b36);t.setPadding(0,dp(4),0,dp(4));return t;}
    private Button button(String title){Button b=new Button(activity);b.setText(title);b.setAllCaps(false);b.setTextSize(13);b.setMinWidth(0);b.setMinimumWidth(0);return b;}
    private final Runnable tick=new Runnable(){public void run(){if(dialog!=null&&dialog.isShowing()){render();ui.postDelayed(this,200);}}};
    public void show(){
        if(dialog!=null)return;frozen=false;saved=null;
        dialog=new Dialog(activity);dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout outer=new LinearLayout(activity);outer.setOrientation(1);outer.setPadding(dp(12),dp(12),dp(12),dp(12));outer.setBackgroundColor(0xfff3f5f7);
        outer.setOnApplyWindowInsetsListener((v,i)->{
            if(android.os.Build.VERSION.SDK_INT>=30){android.graphics.Insets x=i.getInsets(android.view.WindowInsets.Type.systemBars());v.setPadding(dp(12)+x.left,dp(12)+x.top,dp(12)+x.right,dp(12)+x.bottom);}
            else v.setPadding(dp(12)+i.getSystemWindowInsetLeft(),dp(12)+i.getSystemWindowInsetTop(),dp(12)+i.getSystemWindowInsetRight(),dp(12)+i.getSystemWindowInsetBottom());return i;
        });
        LinearLayout root=new LinearLayout(activity);root.setOrientation(1);
        LinearLayout header=new LinearLayout(activity);header.setGravity(android.view.Gravity.CENTER_VERTICAL);
        header.addView(text("真实 PUL / DIR",22),new LinearLayout.LayoutParams(0,-2,1));
        Button halt=button("停止"),close=button("关闭");halt.setTextColor(0xffc03732);
        header.addView(halt);header.addView(close);outer.addView(header);halt.setOnClickListener(v->stop.run());close.setOnClickListener(v->dismiss());
        ScrollView content=new ScrollView(activity);content.addView(root);outer.addView(content,new LinearLayout.LayoutParams(-1,0,1));
        root.addView(text("STM32 PA8 → S3 G6 · PB0 → G7 · 共地\n仅接 3.3V 逻辑；纵轴为 0/1，不测电压幅值。",12));
        status=text("等待采集",12);root.addView(status);
        LinearLayout controls=new LinearLayout(activity);root.addView(controls);
        Button pause=button("冻结显示"),clear=button("清空"),copy=button("复制边沿 CSV");
        controls.addView(pause,new LinearLayout.LayoutParams(0,dp(48),1));controls.addView(clear,new LinearLayout.LayoutParams(0,dp(48),.7f));controls.addView(copy,new LinearLayout.LayoutParams(0,dp(48),1.4f));
        Spinner zoom=new Spinner(activity);zoom.setAdapter(new ArrayAdapter<String>(activity,android.R.layout.simple_spinner_dropdown_item,new String[]{"100 ms 全窗口","25 ms 放大","10 ms 放大"}));root.addView(zoom);
        SeekBar position=new SeekBar(activity);position.setMax(100);position.setEnabled(false);root.addView(position);
        plot=new PulseView(activity);root.addView(plot,new LinearLayout.LayoutParams(-1,dp(260)));
        measurements=text("",12);root.addView(measurements);
        root.addView(text("每段采集 100 ms，最多 512 个边沿；段与段之间有间隙。冻结不停止接收，也不停止电机。无边沿不能证明已接线。",11));
        pause.setOnClickListener(v->{frozen=!frozen;saved=frozen?history.latest():null;pause.setText(frozen?"继续显示":"冻结显示");render();});
        clear.setOnClickListener(v->{history.clear();saved=null;render();});
        copy.setOnClickListener(v->{PulseHistory.Capture s=frozen?saved:history.latest();if(s==null){Toast.makeText(activity,"暂无完整采样",Toast.LENGTH_SHORT).show();return;}
            ClipboardManager manager=(ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if(manager!=null){manager.setPrimaryClip(ClipData.newPlainText("PUL DIR edges",s.csv()));Toast.makeText(activity,"已复制真实边沿时间",Toast.LENGTH_SHORT).show();}});
        zoom.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){
            public void onItemSelected(AdapterView<?> p,View v,int n,long id){span=new double[]{100,25,10}[n];position.setEnabled(n!=0);offset=(100-span)*position.getProgress()/100.0;render();}
            public void onNothingSelected(AdapterView<?> p){}
        });
        position.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onProgressChanged(SeekBar b,int p,boolean user){offset=(100-span)*p/100.0;render();}
            public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}
        });
        dialog.setContentView(outer);dialog.setOnDismissListener(d->{ui.removeCallbacks(tick);dialog=null;});dialog.show();
        Window w=dialog.getWindow();if(w!=null)w.setLayout(WindowManager.LayoutParams.MATCH_PARENT,WindowManager.LayoutParams.MATCH_PARENT);
        outer.requestApplyInsets();ui.post(tick);
    }
    private void render(){
        if(dialog==null||plot==null)return;PulseHistory.Capture s=frozen?saved:history.latest();
        status.setText((frozen?"显示冻结 · ":"")+history.state()+(s==null?"":"\n当前显示 #"+s.id+" · "+Math.max(0,SystemClock.elapsedRealtime()-s.receivedAt)+" ms 前接收"));
        plot.samples(s,offset,span);
        measurements.setText(s==null?"等待有效的采样头、边沿数据和结束校验帧。":!s.available()?"请更新并检查 S3 采集固件。":
            String.format(Locale.ROOT,"窗口 %.3f ms · 硬件计数时钟 %d Hz\n",s.durationMs(),s.hz)+s.measurements());
    }
    public void dismiss(){if(dialog!=null)dialog.dismiss();}
}
