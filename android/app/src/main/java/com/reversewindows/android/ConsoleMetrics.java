package com.reversewindows.android;

import android.app.ActivityManager;
import android.app.AlertDialog;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Build;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.view.FrameMetrics;
import android.view.View;
import android.view.Window;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.text.DateFormat;
import java.util.Date;
import java.util.Locale;

final class ConsoleMetrics {
    private final MainActivity a;
    private final PresentationHistory history=new PresentationHistory();
    private final TextView fps,intervals,episodes,phone,mode;
    private final Plot plot;
    private long sampleCpu,sampleMs,lastSignal,windowFrames,lateFrames;
    private double cpu=Double.NaN;
    private long pss=-1;
    private int cap;
    private boolean running,consoleVisible=true,foreground;
    private final Window.OnFrameMetricsAvailableListener frameTimes=(window,metrics,dropped)->{
        if(!foreground||Build.VERSION.SDK_INT<31)return;
        long deadline=metrics.getMetric(FrameMetrics.DEADLINE),total=metrics.getMetric(FrameMetrics.TOTAL_DURATION);
        if(total>=0&&deadline>0){++windowFrames;if(total>deadline)++lateFrames;}
    };
    ConsoleMetrics(MainActivity a,LinearLayout parent,int cap){
        this.a=a;this.cap=cap;
        mode=a.label("Sesión local · Último minuto de consola",13,MainActivity.MUTED);a.add(parent,mode,4);
        fps=line(parent,"Imágenes de la sesión","Imágenes nuevas de la consola enviadas al compositor por segundo. Un escritorio quieto sin actividad esperada se muestra como un hueco.");
        intervals=line(parent,"Intervalos entre imágenes","P50 y P95 son los percentiles de los intervalos dentro de la muestra. Un intervalo lento supera 1,5 veces el presupuesto de imagen.");
        plot=new Plot(a);parent.addView(plot,new LinearLayout.LayoutParams(-1,a.dp(100)));
        a.add(parent,a.label("Verde · FPS     Ámbar · intervalo P95 (ms)     Línea discontinua · objetivo / presupuesto",11,MainActivity.MUTED),2);
        episodes=line(parent,"Episodios","Con actividad esperada, tres intervalos por debajo del 80 % abren un episodio. Tres al 90 % o más confirman recuperación. Las pausas y las muestras ausentes interrumpen la evaluación.");
        episodes.setOnClickListener(v->showEpisodes());
        phone=line(parent,"Recursos de Android","CPU del proceso por tiempo real (100 % = un núcleo), PSS de la última muestra del motor, RAM disponible y presión según Android. Los cuadros tardíos corresponden a la ventana Android.");
        a.add(parent,a.action("Fuentes y disponibilidad",false,()->new AlertDialog.Builder(a).setTitle("Fuentes de la sesión")
            .setMessage("Vídeo local del invitado y reloj monotónico de Android. El refresco limita el objetivo solicitado. La tasa del motor Éter se mide por separado en la pestaña CPU.\n\nLa sesión funciona en el teléfono. RTT, pérdida y decodificación de vídeo remoto no se aplican. El presentador no expone descartes del compositor ni una correspondencia entre cada entrada y su imagen.")
            .setPositiveButton("Cerrar",null).show()),6);
    }
    private TextView line(LinearLayout p,String title,String detail){
        TextView v=a.label(title,14,MainActivity.TEXT);v.setPadding(a.dp(9),a.dp(7),a.dp(9),a.dp(7));v.setBackground(a.panel(0xff202a32,7,0xff3c4852));a.add(p,v,6);
        v.setOnClickListener(x->new AlertDialog.Builder(a).setTitle(title).setMessage(detail).setPositiveButton("Cerrar",null).show());return v;
    }
    private static String n(double x){return Double.isFinite(x)?String.format(Locale.getDefault(),"%.1f",x):"—";}
    private static String time(long wall){return DateFormat.getTimeInstance(DateFormat.MEDIUM).format(new Date(wall));}
    private float refresh(){android.view.Display d=a.getWindowManager().getDefaultDisplay();return d==null?0:d.getRefreshRate();}
    void cap(int fps){cap=fps;}
    void running(boolean value){running=value;}
    void visible(boolean value){consoleVisible=value;if(!value)history.reset(System.currentTimeMillis());}
    void signal(JSONObject diagnostic){lastSignal=SystemClock.elapsedRealtime();long k=diagnostic.optLong("android_pss_kib",-1);if(k>0)pss=k;}
    void clear(){history.clear();pss=-1;lastSignal=0;windowFrames=lateFrames=0;}
    void input(){if(foreground&&running&&consoleVisible)history.input(SystemClock.elapsedRealtimeNanos());}
    void presented(){if(foreground&&running&&consoleVisible)history.presented(SystemClock.elapsedRealtimeNanos());}
    void start(){
        if(foreground)return;foreground=true;sampleMs=SystemClock.elapsedRealtime();sampleCpu=Process.getElapsedCpuTime();
        a.getWindow().addOnFrameMetricsAvailableListener(frameTimes,new android.os.Handler(android.os.Looper.getMainLooper()));
        timer.run();
    }
    void stop(){if(!foreground)return;foreground=false;a.getWindow().removeOnFrameMetricsAvailableListener(frameTimes);
        plot.removeCallbacks(timer);history.reset(System.currentTimeMillis());}
    private final Runnable timer=new Runnable(){public void run(){
        if(!foreground)return;
        long now=SystemClock.elapsedRealtime(),ns=SystemClock.elapsedRealtimeNanos(),proc=Process.getElapsedCpuTime();
        if(now>sampleMs)cpu=100.0*(proc-sampleCpu)/(now-sampleMs);
        sampleCpu=proc;sampleMs=now;
        int goal=Math.max(1,Math.min(cap,Math.round(refresh())));
        history.tick(ns,System.currentTimeMillis(),goal,consoleVisible&&running,now-lastSignal<3500);
        if(plot.isShown())render();
        plot.postDelayed(this,1000);
    }};
    void render(){
        PresentationHistory.Sample s=history.samples.peekLast();
        String src=Build.VERSION.SDK_INT>=29?"al compositor":"dibujadas por Android";
        mode.setText("Sesión local · "+(s==null?"Sin muestra de consola":time(s.at))+" · "+(running?"Activa":"En espera"));
        fps.setText("Imágenes "+src+"   "+(s==null?"—":n(s.fps))+" FPS     Objetivo   "+Math.min(cap,Math.round(refresh()))+" FPS");
        intervals.setText("Intervalos   P50 "+(s==null?"—":n(s.p50))+" ms    P95 "+(s==null?"—":n(s.p95))+
                " ms    Lentos "+(s==null?"—":n(s.slow))+" %    n = "+(s==null?0:s.intervals));
        episodes.setText("Episodios de baja fluidez   "+history.episodes.size()+"     ›");
        ActivityManager m=a.getSystemService(ActivityManager.class);ActivityManager.MemoryInfo ram=new ActivityManager.MemoryInfo();
        if(m!=null)m.getMemoryInfo(ram);
        PowerManager pm=a.getSystemService(PowerManager.class);
        String[] names={"normal","leve","moderado","severo","crítico","emergencia","apagado"};
        int thermal=Build.VERSION.SDK_INT>=29&&pm!=null?pm.getCurrentThermalStatus():-1;
        phone.setText("Android    CPU "+n(cpu)+" %    PSS "+(pss>0?pss/1024+" MiB":"—")+"    RAM disponible "+(m==null?"—":ram.availMem/1048576+" MiB")+
                (ram.lowMemory?" · presión alta":"")+"\nTérmico "+(thermal>=0&&thermal<names.length?names[thermal]:"—")+
                "    Refresco "+n(refresh())+" Hz    Cuadros tardíos "+(windowFrames>0?n(100.0*lateFrames/windowFrames)+" % ("+lateFrames+"/"+windowFrames+")":"—"));
        plot.invalidate();
    }
    private void showEpisodes(){
        StringBuilder out=new StringBuilder();
        for(PresentationHistory.Episode e:history.episodes){
            out.append(time(e.start)).append(" · ").append(e.end==0?"En curso":((e.end-e.start)/1000)+" s")
               .append(" · mínimo ").append(n(e.worst)).append(" FPS")
               .append(e.interrupted?" · observación interrumpida":"").append('\n');
        }
        new AlertDialog.Builder(a).setTitle("Episodios").setMessage(out.length()==0?"Sin episodios registrados en esta sesión.":out)
                .setPositiveButton("Cerrar",null).show();
    }
    private final class Plot extends View{
        final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        Plot(MainActivity a){super(a);setContentDescription("Últimos 60 intervalos de FPS y P95");}
        @Override protected void onDraw(Canvas c){
            float w=getWidth(),h=getHeight(),margin=a.dp(8),span=h-2*margin;
            c.drawColor(0xff131c23);paint.setStrokeWidth(a.dp(1));paint.setColor(0xff394653);
            for(int i=0;i<=4;i++)c.drawLine(margin,margin+span*i/4,w-margin,margin+span*i/4,paint);
            float ref=margin+span*.5f;paint.setColor(0xff8e9ca2);
            for(float x=margin;x<w-margin;x+=a.dp(9))c.drawLine(x,ref,x+a.dp(4),ref,paint);
            int i=60-history.samples.size();float prevX=0,prevF=Float.NaN,prevT=Float.NaN;
            for(PresentationHistory.Sample s:history.samples){float x=margin+(w-2*margin)*i++/59f;
                float f=margin+span*(1-Math.min(1,(float)(s.fps/(2*s.target))));
                float t=margin+span*(1-Math.min(1,(float)(s.p95*s.target/2000)));
                paint.setStrokeWidth(a.dp(2));paint.setColor(0xff73e6bf);
                if(Float.isFinite(f)){if(Float.isFinite(prevF))c.drawLine(prevX,prevF,x,f,paint);else c.drawPoint(x,f,paint);}
                paint.setColor(0xffffbc6c);
                if(Float.isFinite(t)){if(Float.isFinite(prevT))c.drawLine(prevX,prevT,x,t,paint);else c.drawPoint(x,t,paint);}
                prevX=x;prevF=f;prevT=t;
            }
        }
    }
}
