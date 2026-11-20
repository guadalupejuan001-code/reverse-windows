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
    private final TextView fps,intervals,episodes,phone,mode,goalValue,historyList;
    final LinearLayout motor;
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
    ConsoleMetrics(MainActivity a,ConsolePage window,int cap){
        this.a=a;this.cap=cap;
        LinearLayout parent=window.tab("Resumen","Rendimiento");
        motor=window.tab("Motor","Motor de sesión");
        LinearLayout network=window.tab("Red","Conexión");
        LinearLayout galaxy=window.tab("Galaxy","Recursos de Android");
        LinearLayout historyPanel=window.tab("Episodios","Episodios de baja fluidez");
        a.add(network,a.label("Motor local en este dispositivo",17,MainActivity.TEXT),10);
        a.add(network,a.label("La imagen se entrega directamente a Android. Esta sesión no usa transporte de red ni un decodificador remoto.",14,MainActivity.MUTED),8);
        mode=a.label("Sesión local · Último minuto de consola",13,MainActivity.MUTED);a.add(parent,mode,4);
        LinearLayout cards=new LinearLayout(a);a.add(parent,cards,6);
        fps=tile(cards,"Último segundo","Imágenes nuevas de la consola enviadas al compositor por segundo. Un escritorio quieto sin actividad esperada se muestra como un hueco.");
        goalValue=tile(cards,"Objetivo efectivo","Menor valor entre el límite configurado del presentador y el refresco actual de la pantalla.");
        intervals=tile(cards,"Intervalo P95","El 95 % de los intervalos observados entre imágenes está por debajo de este valor. Se indican el número de intervalos y su mediana en el detalle.");
        intervals.setOnClickListener(v->{
            PresentationHistory.Sample last=history.samples.peekLast();
            new AlertDialog.Builder(a).setTitle("Intervalos entre imágenes").setMessage(last==null?"Sin muestras.":
                "Mediana "+n(last.p50)+" ms · P95 "+n(last.p95)+" ms · "+last.intervals+" intervalos.\n"+
                "Intervalos por encima de 1,5 presupuestos de imagen: "+n(last.slow)+" %.").setPositiveButton("Cerrar",null).show();
        });
        a.add(parent,a.label("Ritmo de la sesión",14,MainActivity.TEXT),9);
        plot=new Plot(a);parent.addView(plot,new LinearLayout.LayoutParams(-1,a.dp(123)));
        a.add(parent,a.label("Últimos 60 s · Línea discontinua: objetivo de FPS o presupuesto de imagen",11,MainActivity.MUTED),2);
        episodes=line(parent,"Episodios","Tres intervalos por debajo del 80 % abren un episodio. Tres al 90 % o más confirman la recuperación.");
        episodes.setOnClickListener(v->window.select(4));
        historyList=line(historyPanel,"Historial de la sesión","Hora local, duración y mínimo de FPS en cada episodio.");
        historyList.setOnClickListener(v->showEpisodes());
        phone=line(galaxy,"Recursos de Android","CPU del proceso por tiempo real (100 % = un núcleo), PSS de la última muestra del motor, RAM disponible y presión según Android. Los cuadros tardíos corresponden a la ventana Android.");
        a.add(motor,a.action("Fuentes y disponibilidad",false,()->new AlertDialog.Builder(a).setTitle("Fuentes de la sesión")
            .setMessage("Vídeo local del invitado y reloj monotónico de Android. El refresco limita el objetivo solicitado. La tasa del motor Éter se mide por separado en la pestaña CPU.\n\nLa sesión funciona en el teléfono. RTT, pérdida y decodificación de vídeo remoto no se aplican. El presentador no expone descartes del compositor ni una correspondencia entre cada entrada y su imagen.")
            .setPositiveButton("Cerrar",null).show()),6);
    }
    private TextView tile(LinearLayout row,String name,String detail){
        LinearLayout tile=new LinearLayout(a);tile.setOrientation(LinearLayout.VERTICAL);tile.setPadding(a.dp(8),a.dp(6),a.dp(5),a.dp(6));
        tile.setBackground(a.panel(0xff1d2a33,7,0xff364650));
        LinearLayout.LayoutParams size=new LinearLayout.LayoutParams(0,-2,1);size.rightMargin=a.dp(4);row.addView(tile,size);
        tile.addView(a.label(name,11,MainActivity.MUTED));TextView value=a.label("—",19,0xff84e5c6);tile.addView(value);
        tile.setOnClickListener(v->new AlertDialog.Builder(a).setTitle(name).setMessage(detail).setPositiveButton("Cerrar",null).show());
        return value;
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
        if(mode.isShown()||phone.isShown()||historyList.isShown()||motor.isShown())render();
        plot.postDelayed(this,1000);
    }};
    void render(){
        PresentationHistory.Sample s=history.samples.peekLast();
        String src=Build.VERSION.SDK_INT>=29?"al compositor":"dibujadas por Android";
        mode.setText("Sesión local · "+(s==null?"Sin muestra de consola":time(s.at))+" · "+(running?"Activa":"En espera"));
        fps.setText((s==null?"—":n(s.fps))+" FPS");fps.setContentDescription("Imágenes "+src+": "+fps.getText());
        goalValue.setText(Math.min(cap,Math.round(refresh()))+" FPS");
        intervals.setText((s==null?"—":n(s.p95))+" ms");
        PresentationHistory.Episode last=history.episodes.peekLast();
        episodes.setText(last==null?"Última caída   ·   Sin episodios registrados":"Última caída   ·   "+time(last.start)+"    "+n(last.worst)+" FPS mínimos    ›");
        StringBuilder historyText=new StringBuilder();
        for(PresentationHistory.Episode e:history.episodes){historyText.append(time(e.start)).append("    ")
                .append(e.end==0?"En curso":((e.end-e.start)/1000)+" s").append("    Mínimo ").append(n(e.worst))
                .append(" FPS").append(e.interrupted?" · interrumpida":"").append('\n');}
        historyList.setText(historyText.length()==0?"Sin episodios registrados en esta sesión.":historyText);
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
            float w=getWidth(),h=getHeight(),d=getResources().getDisplayMetrics().density;
            float left=43*d,right=w-8*d,top=10*d,bottom=h-16*d,half=(bottom-top)/2;
            c.drawColor(0xff19252e);paint.setTextSize(9*d);
            float[] origin={top,top+half};int[] colors={0xff75e5c2,0xffb9b1ff};
            for(int channel=0;channel<2;channel++){
                float high=origin[channel]+4*d,base=origin[channel]+half-4*d,mid=(high+base)/2;
                paint.setStrokeWidth(d);paint.setColor(0xff465661);c.drawLine(left,base,right,base,paint);
                PresentationHistory.Sample last=history.samples.peekLast();int target=last==null?Math.max(1,Math.min(cap,Math.round(refresh()))):last.target;
                paint.setColor(colors[channel]);c.drawText(channel==0?"FPS":"P95 ms",3*d,high+4*d,paint);
                paint.setColor(0xffa8bac6);c.drawText(channel==0?""+target:n(1000.0/target),3*d,mid+5*d,paint);
                for(float x=left;x<right;x+=9*d)c.drawLine(x,mid,Math.min(x+4*d,right),mid,paint);
                int i=60-history.samples.size();float px=0,py=Float.NaN;
                for(PresentationHistory.Sample sample:history.samples){
                    float x=left+(right-left)*i++/59f;
                    float ratio=channel==0?(float)(sample.fps/(2*sample.target)):(float)(sample.p95*sample.target/2000);
                    float y=base-(base-high)*Math.min(1,ratio);
                    if(channel==0 && Double.isFinite(sample.fps) && sample.fps<sample.target*.8){
                        paint.setColor(0x38694e31);c.drawRect(x-(right-left)/118,high,x+(right-left)/118,base,paint);
                    }
                    paint.setStrokeWidth(1.5f*d);paint.setColor(colors[channel]);
                    if(Float.isFinite(y)){if(Float.isFinite(py))c.drawLine(px,py,x,y,paint);else c.drawPoint(x,y,paint);}
                    px=x;py=y;
                }
            }
            paint.setColor(0xffa8bac6);
            for(int t=0;t<=60;t+=15)c.drawText(t+" s",left+(right-left)*t/60-(t==60?14*d:0),h-2*d,paint);

        }
    }
}
