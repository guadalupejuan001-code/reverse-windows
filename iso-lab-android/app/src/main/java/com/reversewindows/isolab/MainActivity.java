package com.reversewindows.isolab;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * User-owned ISO files only. ACTION_OPEN_DOCUMENT grants read access and
 * ACTION_CREATE_DOCUMENT supplies a separate output. No network permission.
 */
public final class MainActivity extends Activity {
    private static final int PICK_ISO=101, PICK_ARCHIVE=102, SAVE_ARCHIVE=103, SAVE_ISO=104;
    private static final int BG=0xff090F19, PANEL=0xff111D2B, BORDER=0xff284154;
    private static final int TEXT=0xffECF7FB, MUTED=0xffA1B9C6, ACCENT=0xff62E3D3;
    private static final int BUTTON=0xff203648, DANGER=0xffFFB18A;

    private final ExecutorService executor=Executors.newSingleThreadExecutor(r->{
        Thread t=new Thread(r,"REVERSE-ISO-stream");t.setDaemon(true);return t;
    });
    private final AtomicBoolean cancel=new AtomicBoolean(false);
    private Uri isoUri, archiveUri;
    private long isoBytes=-1;
    private boolean busy=false;
    private TextView isoLabel,archiveLabel,status,progressLabel;
    private Button chooseIso,compact,chooseArchive,restore,cancelButton;
    private ProgressBar bar;

    private int dp(float pixels){return Math.round(pixels*getResources().getDisplayMetrics().density);}
    private GradientDrawable surface(int fill,int stroke){
        GradientDrawable d=new GradientDrawable();
        d.setColor(fill);d.setCornerRadius(dp(15));
        if(stroke!=0)d.setStroke(dp(1),stroke);
        return d;
    }
    private TextView text(String value,int sp,int color,boolean bold){
        TextView t=new TextView(this);
        t.setText(value);t.setTextSize(sp);t.setTextColor(color);
        t.setLineSpacing(dp(3),1.06f);
        if(bold)t.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        return t;
    }
    private LinearLayout.LayoutParams space(int pixels){
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);
        p.topMargin=dp(pixels);
        return p;
    }
    private Button button(String value,boolean primary){
        Button b=new Button(this);
        b.setText(value);b.setTextSize(14);b.setAllCaps(false);
        b.setTextColor(primary?BG:TEXT);
        b.setBackground(surface(primary?ACCENT:BUTTON,primary?0:BORDER));
        b.setPadding(dp(12),dp(12),dp(12),dp(12));
        b.setMinHeight(dp(49));
        return b;
    }
    private LinearLayout panel(LinearLayout root,String title,String info){
        LinearLayout p=new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setPadding(dp(15),dp(17),dp(15),dp(17));
        p.setBackground(surface(PANEL,BORDER));
        p.addView(text(title,17,TEXT,true));
        TextView desc=text(info,12,MUTED,false);
        p.addView(desc,space(7));
        root.addView(p,space(13));
        return p;
    }
    @Override protected void onCreate(Bundle bundle){
        super.onCreate(bundle);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);

        ScrollView scroll=new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        scroll.setBackgroundColor(BG);
        scroll.setOnApplyWindowInsetsListener((v,insets)->{
            int top,bottom;
            if(Build.VERSION.SDK_INT>=30){
                Insets safe=insets.getInsets(WindowInsets.Type.systemBars());
                top=safe.top;bottom=safe.bottom;
            }else{
                top=insets.getSystemWindowInsetTop();
                bottom=insets.getSystemWindowInsetBottom();
            }
            scroll.setPadding(dp(17),top+dp(16),dp(17),bottom+dp(25));
            return insets;
        });
        LinearLayout root=new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(0,0,0,dp(20));
        scroll.addView(root,new ScrollView.LayoutParams(-1,-2));

        TextView kicker=text("REVERSE  /  EXPERIMENTOS",11,ACCENT,true);
        root.addView(kicker);
        root.addView(text("ISO LAB",30,TEXT,true),space(6));
        root.addView(text("Descomponer. Compactar. Reconstruir.",14,MUTED,false),space(4));

        LinearLayout a=panel(root,"01   ISO ORIGINAL",
            "Selecciona una ISO desde el teléfono. Se inspecciona su cabecera sin cargar el archivo completo.");
        chooseIso=button("Seleccionar ISO",true);
        isoLabel=text("Ninguna ISO seleccionada",12,MUTED,false);
        a.addView(chooseIso,space(13));
        a.addView(isoLabel,space(10));
        chooseIso.setOnClickListener(v->openDocument(PICK_ISO));

        LinearLayout b=panel(root,"02   DESCOMPONER Y COMPACTAR",
            "Bloques de 64 KiB: ceros, patrones periódicos, duplicados y compresión sin pérdida. Se crea un archivo .riso.");
        compact=button("Crear archivo compacto .riso",false);
        b.addView(compact,space(11));
        compact.setOnClickListener(v->createDocument(SAVE_ARCHIVE,
            suggested(isoUri==null?"Windows10.iso":label(isoUri),".iso",".riso")));

        LinearLayout c=panel(root,"03   RECONSTRUIR ISO",
            "Selecciona un .riso y restaura el contenido original. Se comprueban tamaños y SHA-256 antes de dar éxito.");
        chooseArchive=button("Seleccionar archivo .riso",false);
        archiveLabel=text("Ningún archivo compacto seleccionado",12,MUTED,false);
        restore=button("Reconstruir archivo .iso",true);
        c.addView(chooseArchive,space(10));
        c.addView(archiveLabel,space(7));
        c.addView(restore,space(11));
        chooseArchive.setOnClickListener(v->openDocument(PICK_ARCHIVE));
        restore.setOnClickListener(v->createDocument(SAVE_ISO,"reconstruida.iso"));

        LinearLayout p=panel(root,"04   PROCESO Y RESULTADOS",
            "Todo ocurre en este teléfono. La aplicación no usa Internet ni modifica la ISO de entrada.");
        bar=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        p.addView(bar,space(11));
        progressLabel=text("Preparado para analizar.",12,MUTED,false);
        p.addView(progressLabel,space(8));
        cancelButton=button("Cancelar operación",false);
        p.addView(cancelButton,space(9));
        cancelButton.setOnClickListener(v->{
            cancel.set(true);
            progressLabel.setText("Cancelación solicitada; cerrando el bloque actual...");
        });
        status=text("Sin resultados todavía.",13,TEXT,false);
        p.addView(status,space(12));

        TextView foot=text("Experimental · Formato REISO01 · SHA-256 · Los ahorros dependen del contenido de cada ISO. "+
            "No se afirma integración activa con Ether o QSSD en esta primera referencia.",11,MUTED,false);
        foot.setGravity(Gravity.CENTER);
        root.addView(foot,space(20));
        setContentView(scroll);
        refresh();
    }
    private void refresh(){
        chooseIso.setEnabled(!busy);
        chooseArchive.setEnabled(!busy);
        compact.setEnabled(!busy && isoUri!=null);
        restore.setEnabled(!busy && archiveUri!=null);
        cancelButton.setEnabled(busy);
        bar.setVisibility(busy?View.VISIBLE:View.INVISIBLE);
        cancelButton.setVisibility(busy?View.VISIBLE:View.GONE);
    }
    private void openDocument(int request){
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent,request);
    }
    private void createDocument(int request,String name){
        if(busy)return;
        Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        intent.putExtra(Intent.EXTRA_TITLE,name);
        intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        startActivityForResult(intent,request);
    }
    private static String suggested(String oldName,String oldExt,String newExt){
        if(oldName.toLowerCase(Locale.ROOT).endsWith(oldExt))
            return oldName.substring(0,oldName.length()-oldExt.length())+newExt;
        return oldName+newExt;
    }
    private String label(Uri uri){
        if(uri==null)return "(ninguno)";
        try(Cursor cursor=getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){
            if(cursor!=null && cursor.moveToFirst()){
                int index=cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if(index>=0 && !cursor.isNull(index))return cursor.getString(index);
            }
        }catch(Exception ignored){}
        return "Archivo seleccionado";
    }
    private long size(Uri uri){
        try(Cursor cursor=getContentResolver().query(uri,
                new String[]{OpenableColumns.SIZE},null,null,null)){
            if(cursor!=null && cursor.moveToFirst()){
                int index=cursor.getColumnIndex(OpenableColumns.SIZE);
                if(index>=0 && !cursor.isNull(index))return cursor.getLong(index);
            }
        }catch(Exception ignored){}
        return -1;
    }
    private void preserveRead(Uri uri){
        try{getContentResolver().takePersistableUriPermission(uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION);}
        catch(SecurityException ignored){}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK || data==null || data.getData()==null)return;
        Uri uri=data.getData();
        if(request==PICK_ISO){
            isoUri=uri;isoBytes=size(uri);preserveRead(uri);
            isoLabel.setText(label(uri)+"\n"+(isoBytes>=0?format(isoBytes):"Tamaño no informado")+
                             "\nAnalizando cabecera...");
            executor.execute(()->{
                try(InputStream source=getContentResolver().openInputStream(uri)){
                    if(source==null)throw new IllegalStateException("No se pudo abrir la ISO");
                    String info=IsoInspector.quick(source);
                    runOnUiThread(()->{if(uri.equals(isoUri))isoLabel.setText(
                        label(uri)+"\n"+(isoBytes>=0?format(isoBytes):"Tamaño no informado")+"\n"+info);});
                }catch(Exception ex){
                    runOnUiThread(()->{if(uri.equals(isoUri))isoLabel.setText(
                        label(uri)+"\nCabecera: "+message(ex));});
                }
            });
        }else if(request==PICK_ARCHIVE){
            archiveUri=uri;preserveRead(uri);
            archiveLabel.setText(label(uri)+" · "+format(size(uri)));
        }else if(request==SAVE_ARCHIVE && isoUri!=null){
            perform(true,isoUri,uri,isoBytes);
        }else if(request==SAVE_ISO && archiveUri!=null){
            perform(false,archiveUri,uri,size(archiveUri));
        }
        refresh();
    }
    private static String format(long bytes){
        if(bytes<0)return "Tamaño desconocido";
        if(bytes<1024)return bytes+" B";
        return String.format(Locale.US,"%.2f MiB",bytes/1048576.0);
    }
    private static String message(Throwable e){
        String s=e.getMessage();
        return s==null?e.getClass().getSimpleName():s;
    }
    private void removePartial(Uri output){
        try{DocumentsContract.deleteDocument(getContentResolver(),output);}
        catch(Exception ignored){}
    }
    private void perform(boolean packing,Uri inputUri,Uri outputUri,long expectedSize){
        if(busy)return;
        busy=true;cancel.set(false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        status.setText("Ejecutando "+(packing?"compactación":"reconstrucción")+"...");
        progressLabel.setText("Iniciando lectura por bloques...");
        bar.setProgress(0);bar.setIndeterminate(expectedSize<=0);
        refresh();
        executor.execute(()->{
            final long[] lastUpdate={0L};
            IsoArchive.Progress onProgress=(done,total)->{
                long now=SystemClock.uptimeMillis();
                if(now-lastUpdate[0]<120 && !(total>0 && done>=total))return;
                lastUpdate[0]=now;
                runOnUiThread(()->{
                    if(!busy)return;
                    progressLabel.setText(format(done)+" / "+format(total));
                    if(total>0){
                        bar.setIndeterminate(false);
                        bar.setProgress((int)Math.min(1000,1000.0*done/total));
                    }
                });
            };
            try{
                IsoArchive.Result result;
                if(packing){
                    try(InputStream in=getContentResolver().openInputStream(inputUri);
                        OutputStream out=getContentResolver().openOutputStream(outputUri,"wt")){
                        if(in==null || out==null)throw new IllegalStateException("Permiso de archivo incompleto");
                        result=IsoArchive.compact(in,out,expectedSize,onProgress,cancel::get);
                    }
                }else{
                    try(ParcelFileDescriptor file=getContentResolver().openFileDescriptor(inputUri,"r")){
                        if(file==null)throw new IllegalStateException("Archivo .riso no accesible");
                        try(FileInputStream stream=new FileInputStream(file.getFileDescriptor());
                            OutputStream out=getContentResolver().openOutputStream(outputUri,"wt")){
                            if(out==null)throw new IllegalStateException("No se pudo crear la ISO");
                            FileChannel channel=stream.getChannel();
                            if(channel.size()==0)throw new IllegalStateException(
                                "Este proveedor no admite lectura aleatoria de archivos. "+
                                "Guarda el .riso en el almacenamiento local e inténtalo de nuevo.");
                            result=IsoArchive.restore(channel,out,channel.size(),onProgress,cancel::get);
                        }
                    }
                }
                if(cancel.get())throw new IllegalStateException("Operación cancelada");
                String details=(packing?
                    "Archivo compacto creado.\nISO: "+format(result.originalBytes)+
                    "\nRISO: "+format(result.archiveBytes)+
                    String.format(Locale.US,"\nReducción: %.2f%%",result.reductionPercent())+
                    "\nBloques duplicados: "+result.duplicate+
                    "\nBloques de ceros: "+result.zero+
                    "\nPatrones: "+result.periodic+
                    "\nBloques Deflate: "+result.deflated:
                    "ISO reconstruida.\nTamaño: "+format(result.originalBytes)+
                    "\nSHA-256 original verificado: SÍ")+
                    "\nSHA-256 ISO:\n"+result.sha256+
                    "\n\nLos datos quedan únicamente en el archivo de salida que elegiste.";
                runOnUiThread(()->{
                    busy=false;refresh();bar.setIndeterminate(false);bar.setProgress(1000);
                    progressLabel.setText("Completado");
                    status.setText(details);
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                });
            }catch(Exception ex){
                removePartial(outputUri);
                String reason=message(ex);
                runOnUiThread(()->{
                    busy=false;refresh();bar.setIndeterminate(false);
                    progressLabel.setText(cancel.get()?"Cancelado":"Error en el proceso");
                    status.setTextColor(DANGER);
                    status.setText(reason+"\nEl archivo de salida parcial no es válido.");
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                });
            }
        });
    }
    @Override protected void onDestroy(){
        cancel.set(true);executor.shutdownNow();
        super.onDestroy();
    }
}
