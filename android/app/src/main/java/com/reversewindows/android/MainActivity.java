package com.reversewindows.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ComponentCallbacks2;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.util.Locale;

public final class MainActivity extends Activity implements VmWorker.Listener, GuestMonitor.Input {
    private static final int ISO_PICKER = 21;
    private static final int DIAGNOSTIC_PICKER = 22;
    private static final int SETTINGS_LOG_TAB = 3;
    private static final long MIB = 1024L * 1024L;
    private static final int BG = 0xff101a20;
    private static final int PANEL = 0xb8132026;
    static final int BORDER = 0x66c4e4eb;
    static final int TEXT = 0xfff1f7f8;
    static final int MUTED = 0xffd6e4e8;
    static final int ACCENT = 0xffbaf6e5;
    static final int ERROR = 0xffffe0bb;
    private static final String[][] HARDWARE_CHECKS = {
            {"cpu_x64", "CPU x64"}, {"cpu_mmx", "MMX"}, {"cpu_sse", "SSE"},
            {"cpu_sse2", "SSE2"}, {"cpu_x87", "x87"},
            {"cpu_pae", "PAE"}, {"cpu_nx", "NX"}, {"cpu_cx16", "CMPXCHG16B"},
            {"cpu_lahf_sahf", "LAHF/SAHF"}, {"cpu_prefetchw", "PREFETCHW"}, {"cpu_fxsr", "FXSR"},
            {"lapic", "Controlador local de interrupciones (xAPIC)"},
            {"ether_quantum_block", "Bloque cuántico Éter · par de Bell"},
            {"quantum_ram_guest", "RAM Éter · lectura y escritura x86"},
            {"ram", "RAM"}, {"pci_ide", "Controlador PCI IDE"}, {"vga", "Video VGA"},
            {"iso_boot", "Arranque desde ISO"}, {"qssd", "Archivo QSSD"},
            {"qssd_ata", "Capacidad QSSD por ATA"},
    };

    private File disk;
    private File dataDisk;
    private SharedPreferences prefs;
    private Uri iso;
    private String isoName;
    private VmWorker worker;
    private GuestMonitor monitor;
    private FrameLayout pages;
    private ScrollView metricPage;
    private ConsoleDeck console;
    private ConsolePage settingsWindow,metricsWindow;
    private ConsoleMetrics liveMetrics;
    private TextView displayValues;
    private int page;
    private VmTelemetry telemetry;
    private TextView sessionSummary;
    private TextView media;
    private TextView storage;
    private TextView status;
    private TextView stats;
    private TextView source;
    private Button selectIso;
    private Button ejectIso;
    private Button createDisk;
    private Button createDataDisk;
    private Button bootIso;
    private Button bootDisk;
    private Button biosTest;
    private Button pause;
    private Switch experimentalClock;
    private Button stop;
    private Button evidence;
    private Button hardwareCheck;
    private Button ramProfile;
    private TextView hardwareResult;
    private LinearLayout controls;
    private LinearLayout primaryActions;
    private LinearLayout setupPanel;
    private boolean pending;
    private boolean session;
    private boolean running;
    private boolean fault;
    private boolean terminal;
    private boolean checkingHardware;
    private boolean requestingDiagnostic;
    private boolean blankTextFrame;
    private boolean primeRam;
    private String lastJson = "";
    private String diagnosticToExport = "";
    private final VmStepRate executionRate = new VmStepRate();
    private final VmStepRate motorRate = new VmStepRate();

    @Override
    public void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        disk = new File(getFilesDir(), "windows.rqvd");
        dataDisk = new File(getFilesDir(), "windows-data.rqvd");
        prefs = getSharedPreferences("vm", MODE_PRIVATE);
        primeRam = prefs.getBoolean("prime_ram_64_gib", true);
        String savedIso = prefs.getString("iso", "");
        if (!savedIso.isEmpty()) {
            iso = Uri.parse(savedIso);
            isoName = prefs.getString("iso_name", "ISO");
        }
        if (savedState != null)
            diagnosticToExport = savedState.getString("diagnosticToExport", "");
        worker = new VmWorker(this, disk, dataDisk, this);
        buildScreen();
        updateControls();
        worker.loadDiagnostic();
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra(
                "com.reversewindows.android.SELF_TEST", false)) {
            start(NativeMachine.BOOT_SELF_TEST, true);
        }
    }

    private void buildScreen() {
        pages=new FrameLayout(this);
        pages.setBackgroundColor(0xff171c22);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        if(Build.VERSION.SDK_INT>=28){
            WindowManager.LayoutParams lp=getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        pages.setOnApplyWindowInsetsListener((v,in)->{
            if(Build.VERSION.SDK_INT>=30){
                android.graphics.Insets safe=in.getInsets(WindowInsets.Type.displayCutout()|
                        WindowInsets.Type.systemBars()|WindowInsets.Type.ime());
                v.setPadding(safe.left,safe.top,safe.right,safe.bottom);
            }else{
                int left=in.getSystemWindowInsetLeft(),top=in.getSystemWindowInsetTop(),right=in.getSystemWindowInsetRight(),bottom=in.getSystemWindowInsetBottom();
                if(Build.VERSION.SDK_INT>=28&&in.getDisplayCutout()!=null){
                    left=Math.max(left,in.getDisplayCutout().getSafeInsetLeft());right=Math.max(right,in.getDisplayCutout().getSafeInsetRight());
                    top=Math.max(top,in.getDisplayCutout().getSafeInsetTop());bottom=Math.max(bottom,in.getDisplayCutout().getSafeInsetBottom());
                }
                v.setPadding(left,top,right,bottom);
            }
            return in;
        });
        monitor=new GuestMonitor(this,this,worker.vbePresentation);

        ScrollView scroll=new ScrollView(this);
        settingsWindow=new ConsolePage(this,scroll,"Configuración",()->showPage(0),"Métricas",()->showPage(2));
        LinearLayout body=settingsWindow.tab("Sesión","Configuración");
        status=label("Máquina apagada",15,TEXT);status.setId(R.id.vm_status);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(body,status,5);
        source=label(BuildConfig.VERSION_NAME+" · Motor local Éter · Invitado x86-64",13,ACCENT);add(body,source,3);
        evidence=action("Diagnóstico",false,this::showDiagnostic);evidence.setId(R.id.vm_copy_diagnostic);add(body,evidence,5);
        setupPanel=card(body,8);
        controls=card(body,8);
        experimentalClock=new Switch(this);experimentalClock.setId(R.id.vm_experimental_clock);
        experimentalClock.setText("Reloj virtual de 4,5 THz");experimentalClock.setTextColor(TEXT);
        experimentalClock.setOnClickListener(v->worker.setExperimentalClock(experimentalClock.isChecked()));add(controls,experimentalClock,0);
        primaryActions=row(controls);
        pause=action("Pausar",false,()->{console.release();worker.setRunning(!running);});
        stop=action("Apagar",false,this::confirmStop);pair(primaryActions,pause,stop);
        LinearLayout diagRow=row(controls);
        pair(diagRow,action("Registro",false,this::openLog),
                action("Teclado de Android",false,()->{showPage(0);monitor.showKeyboard();}));
        LinearLayout display=card(settingsWindow.tab("Pantalla","Pantalla y fluidez"),8);display.addView(label("Señal de vídeo",16,TEXT));
        displayValues=label("",13,MUTED);add(display,displayValues,4);
        int fps=prefs.getInt("presentation_fps",60);
        Button target=action("Objetivo del presentador · "+fps+" FPS",false,()->{});add(display,target,4);
        target.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Objetivo del presentador")
                .setItems(new String[]{"30 FPS","60 FPS","90 FPS"},(dialog,index)->{
                    int value=new int[]{30,60,90}[index];prefs.edit().putInt("presentation_fps",value).apply();
                    worker.setPresentationTarget(value);liveMetrics.cap(value);target.setText("Objetivo del presentador · "+value+" FPS");
                }).show());
        worker.setPresentationTarget(fps);
        LinearLayout inputCard=card(settingsWindow.tab("Controles","Teclado y puntero"),8);
        inputCard.addView(label("Distribución española · ISO de 105 teclas",16,TEXT));
        String initial=prefs.getString("rgb_default_mode","ABC");
        RgbKeyboard.Mode initialMode;
        try{initialMode=RgbKeyboard.Mode.valueOf(initial);}catch(IllegalArgumentException e){initialMode=RgbKeyboard.Mode.ABC;}
        Button initialButton=action("Vista inicial · "+initialMode,false,()->{});add(inputCard,initialButton,4);
        initialButton.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Vista inicial del teclado")
                .setItems(new String[]{"Completo · 105 teclas","ABC · Pulgares","Núm · Pulgares","Fn · Pulgares"},(dialog,index)->{
                    RgbKeyboard.Mode m=RgbKeyboard.Mode.values()[index];prefs.edit().putString("rgb_default_mode",m.name()).apply();
                    console.keyboard.setMode(m);initialButton.setText("Vista inicial · "+m);
                }).show());
        int light=prefs.getInt("rgb_light",70),speed=prefs.getInt("rgb_pointer_speed",10);
        addSlider(inputCard,"Intensidad RGB",light,100,0," %","rgb_light");
        addSlider(inputCard,"Sensibilidad del puntero",speed,20,5,"×","rgb_pointer_speed");
        LinearLayout log=settingsWindow.tab("Registro","Registro del invitado");
        stats=label("",12,MUTED);
        stats.setTypeface(Typeface.MONOSPACE);
        stats.setTextIsSelectable(true);
        add(log,stats,6);
        metricPage=new ScrollView(this);
        metricsWindow=new ConsolePage(this,metricPage,"Métricas",()->showPage(0),"Configuración",()->showPage(1));
        liveMetrics=new ConsoleMetrics(this,metricsWindow,fps);
        LinearLayout metrics=liveMetrics.motor;
        telemetry=new VmTelemetry(this);add(metrics,telemetry,10);
        sessionSummary=label("",12,MUTED);add(metrics,sessionSummary,4);
        sessionSummary.setVisibility(View.GONE);
        console=new ConsoleDeck(this,monitor,new ConsoleDeck.Input(){
            public void key(int ascii,int scan,boolean down){if(canQueueGuestInput()){if(down)liveMetrics.input();worker.key(ascii,scan,down);}}
            public void pointer(int dx,int dy,int buttons){if(canQueueGuestInput()){liveMetrics.input();worker.pointer(dx,dy,buttons);}}
        },initialMode,light,speed/10f,()->showPage(1),()->showPage(2),this::openLog);
        metricsWindow.changed(()->{telemetry.render();liveMetrics.render();});
        settingsWindow.changed(this::updateDisplayValues);
        pages.addView(console);pages.addView(settingsWindow);pages.addView(metricsWindow);

        LinearLayout setup = setupPanel;
        TextView bootTitle = label("Arranque", 16, TEXT);
        bootTitle.setTypeface(Typeface.DEFAULT_BOLD);
        setup.addView(bootTitle);
        media = label("", 13, MUTED);
        media.setSingleLine(true);
        media.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        add(setup, media, 10);
        LinearLayout mediaRow = row(setup);
        ((LinearLayout.LayoutParams) mediaRow.getLayoutParams()).topMargin = dp(5);
        selectIso = action("Elegir ISO o RISO", false, this::pickIso);
        selectIso.setId(R.id.vm_select_iso);
        ejectIso = action("Expulsar ISO", false, this::clearIso);
        pair(mediaRow, selectIso, ejectIso);

        storage = label("", 13, MUTED);
        add(setup, storage, 9);
        ramProfile = action("", false, () -> {
            primeRam = !primeRam;
            prefs.edit().putBoolean("prime_ram_64_gib", primeRam).apply();
            clearHardwareResult();
            updateControls();
        });
        add(setup, ramProfile, 6);

        bootIso = action("Arrancar desde CD/DVD", true, () -> start(NativeMachine.BOOT_ISO, false));
        bootIso.setId(R.id.vm_boot_iso);
        add(setup, bootIso, 12);
        LinearLayout bootRow = row(setup);
        ((LinearLayout.LayoutParams) bootRow.getLayoutParams()).topMargin = dp(6);
        bootDisk = action("Arrancar QSSD", false, () -> start(NativeMachine.BOOT_QSSD, false));
        bootDisk.setId(R.id.vm_boot_disk);
        biosTest = action("Probar BIOS", false, () -> start(NativeMachine.BOOT_SELF_TEST, false));
        biosTest.setId(R.id.vm_bios_test);
        pair(bootRow, bootDisk, biosTest);

        createDisk = action("Crear QSSD de 64 GiB", false, () -> prepareDisk(false));
        createDisk.setId(R.id.vm_create_disk);
        add(setup, createDisk, 6);
        createDataDisk = action("Añadir QSSD de datos de 8 PiB", false, () -> prepareDisk(true));
        createDataDisk.setId(R.id.vm_create_data_disk);
        add(setup, createDataDisk, 6);

        hardwareCheck = action("Comprobar equipo virtual", false, this::checkHardware);
        hardwareCheck.setId(R.id.vm_hardware_check);
        add(setup, hardwareCheck, 12);
        hardwareResult = label("", 13, MUTED);
        hardwareResult.setId(R.id.vm_hardware_result);
        hardwareResult.setVisibility(View.GONE);
        hardwareResult.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        add(setup, hardwareResult, 6);
        setContentView(pages);
        showPage(0);
    }

    private void addSlider(LinearLayout parent,String title,int value,int max,int min,String unit,String preference){
        boolean speed=min>0;
        TextView label=label("",13,MUTED);add(parent,label,8);
        SeekBar bar=new SeekBar(this);bar.setContentDescription(title);bar.setMax(max-min);add(parent,bar,2);
        java.util.function.IntConsumer show=n->label.setText(title+" · "+(speed?String.format(Locale.getDefault(),"%.1f",n/10f):Integer.toString(n))+unit);
        show.accept(value);bar.setProgress(value-min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onStartTrackingTouch(SeekBar b){}
            public void onStopTrackingTouch(SeekBar b){}
            public void onProgressChanged(SeekBar b,int n,boolean user){
                if(!user)return;int level=n+min;show.accept(level);
                prefs.edit().putInt(preference,level).apply();
                if(speed)console.sensitivity(level/10f);else console.keyboard.light(level);
            }
        });
    }
    private void showPage(int next){
        if(page==0&&next!=0)console.dismissInputs();
        page=next;
        console.setVisibility(next==0?View.VISIBLE:View.GONE);
        settingsWindow.setVisibility(next==1?View.VISIBLE:View.GONE);
        metricsWindow.setVisibility(next==2?View.VISIBLE:View.GONE);
        liveMetrics.visible(next==0);
        if(next==1){settingsWindow.select(0);updateDisplayValues();}
        if(next==2){metricsWindow.select(0);telemetry.render();liveMetrics.render();}
        if(next==0)restoreGuestFocus();
    }
    private void updateDisplayValues(){
        displayValues.setText("Salida del invitado · "+monitor.dimensions()+"    Pantalla Android · "+
                getResources().getDisplayMetrics().widthPixels+" × "+getResources().getDisplayMetrics().heightPixels+" px");
    }
    @Override public void onBackPressed(){
        if(page!=0){showPage(0);return;}
        if(console.dismissInputs()){restoreGuestFocus();return;}
        super.onBackPressed();
    }
    private void openLog(){
        showPage(1);
        settingsWindow.select(SETTINGS_LOG_TAB);
    }
    @Override public void onPresented(){liveMetrics.presented();}

    private void pickIso() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(picker, ISO_PICKER);
        } catch (android.content.ActivityNotFoundException error) {
            Toast.makeText(this, "Android no encontró un selector de archivos.", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == DIAGNOSTIC_PICKER) {
            if (result == RESULT_OK && data != null && data.getData() != null &&
                    !diagnosticToExport.isEmpty())
                worker.exportDiagnostic(data.getData(), diagnosticToExport);
            diagnosticToExport = "";
            return;
        }
        if (request != ISO_PICKER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri chosen = data.getData();
        boolean persisted = false;
        try {
            getContentResolver().takePersistableUriPermission(chosen, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            persisted = true;
        } catch (SecurityException ignored) {
            // A transient grant is sufficient for this session.
        }
        if (!chosen.equals(iso)) releaseIsoGrant();
        iso = chosen;
        isoName = displayName(chosen);
        SharedPreferences.Editor editor = prefs.edit();
        if (persisted) editor.putString("iso", chosen.toString()).putString("iso_name", isoName);
        else editor.remove("iso").remove("iso_name");
        editor.apply();
        clearHardwareResult();
        updateControls();
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getString(0);
        } catch (RuntimeException ignored) {
            // The URI can still be opened when the provider does not expose a name.
        }
        return "ISO seleccionada";
    }

    private void releaseIsoGrant() {
        if (iso == null) return;
        try {
            getContentResolver().releasePersistableUriPermission(iso, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // Transient providers have no persistable grant.
        }
    }

    private void clearIso() {
        releaseIsoGrant();
        iso = null;
        isoName = null;
        prefs.edit().remove("iso").remove("iso_name").apply();
        clearHardwareResult();
        updateControls();
    }

    private void prepareDisk(boolean data) {
        new AlertDialog.Builder(this)
                .setTitle(data ? "Crear QSSD de datos" : "Crear disco virtual")
                .setMessage(data
                        ? "Añade 8 PiB lógicos. El archivo crece con lo que guardes. En Windows 10 "
                            + "1709 o posterior, inicializa el disco como GPT y usa formato rápido "
                            + "NTFS con unidades de asignación de 2 MiB."
                        : "El QSSD mostrará 64 GiB al invitado. El archivo del teléfono crece conforme Windows guarda datos.")
                .setNegativeButton("Cancelar", null)
                .setPositiveButton("Crear", (dialog, which) -> {
                    pending = true;
                    status.setText("Creando QSSD…");
                    status.setTextColor(TEXT);
                    updateControls();
                    worker.createDisk(data);
                })
                .show();
    }

    private void start(int bootSource, boolean debug) {
        if (session || pending) return;
        experimentalClock.setChecked(true);
        pending = true;
        fault = false;
        terminal = false;
        blankTextFrame = false;
        executionRate.reset();
        motorRate.reset();
        liveMetrics.clear();
        console.dismissInputs();
        console.keyboard.resetGuest();
        showPage(0);
        monitor.reset();
        monitor.requestFocus();
        telemetry.clear();
        sessionSummary.setVisibility(View.GONE);
        source.setText(switch (bootSource) {
            case NativeMachine.BOOT_ISO -> "BIOS · CD/DVD";
            case NativeMachine.BOOT_QSSD -> "BIOS · QSSD";
            default -> "BIOS · PRUEBA";
        });
        status.setText(bootSource == NativeMachine.BOOT_ISO
                ? "Abriendo la ISO y preparando el QSSD…" : "Cargando medio de arranque…");
        status.setTextColor(TEXT);
        stats.setText("");
        updateControls();
        worker.start(bootSource, iso, debug, primeRam);
        showBootStatus();
    }

    private void checkHardware() {
        if (session || pending) return;
        pending = true;
        checkingHardware = true;
        setHardwareResult("Comprobando equipo virtual y medios seleccionados…", MUTED);
        updateControls();
        worker.checkHardware(iso, primeRam);
    }

    private void clearHardwareResult() {
        hardwareResult.setVisibility(View.GONE);
    }

    private void setHardwareResult(String result, int color) {
        hardwareResult.setText(result);
        hardwareResult.setTextColor(color);
        hardwareResult.setVisibility(View.VISIBLE);
        hardwareResult.post(() -> hardwareResult.requestRectangleOnScreen(new Rect(
                0, 0, hardwareResult.getWidth(), Math.min(hardwareResult.getHeight(), dp(72)))));
    }

    @Override
    public void onHardwareChecked(String json) {
        pending = false;
        checkingHardware = false;
        try {
            JSONObject preflight = new JSONObject(json);
            JSONObject checks = preflight.getJSONObject("checks");
            JSONObject errors = preflight.optJSONObject("errors");
            boolean ready = preflight.getBoolean("ready_for_windows10_install");
            StringBuilder result = new StringBuilder(ready
                    ? "Requisitos comprobados para Windows 10"
                    : "Requisitos pendientes para Windows 10");
            JSONObject ether = preflight.optJSONObject("ether_x86_sessions");
            if (ether != null) {
                result.append("\n\nSesiones Éter x86 · prueba");
                if (!ether.optBoolean("passed")) result.append(" incompleta");
                result.append(String.format(Locale.getDefault(), "\n%d/%d trabajos completos · %d vCPU",
                        ether.optInt("completed_jobs"), ether.optInt("requested_jobs"), ether.optInt("owners")));
                long elapsedNs = ether.optLong("elapsed_ns");
                if (elapsedNs > 0) {
                    result.append(String.format(Locale.getDefault(), " · %.3f ms", elapsedNs / 1_000_000.0));
                }
                long projected = ether.optLong("projected_steps");
                if (projected > 0) {
                    result.append(String.format(Locale.getDefault(), "\n%,d pasos x86 proyectados", projected));
                }
                String detail = ether.optString("error");
                if (!detail.isEmpty()) result.append('\n').append(detail);
                result.append('\n');
            }
            JSONObject ram = preflight.optJSONObject("ram_profile");
            if (ram != null) {
                result.append("\nRAM utilizable por el invitado: ")
                        .append(ram.getLong("usable_bytes") / MIB).append(" MiB")
                        .append("\nLímite de RAM residente en Android: ")
                        .append(ram.getLong("backing_budget_bytes") / MIB).append(" MiB");
                String spill = ram.optString("spill_mode");
                if ("file".equals(spill)) {
                    result.append("\nQSSD temporal para RAM · hasta ")
                            .append(ram.optLong("spill_limit_bytes") / MIB).append(" MiB");
                } else if ("unavailable".equals(spill)) {
                    result.append("\nRespaldo temporal de RAM no disponible.");
                    String spillError = ram.optString("spill_error");
                    if (!spillError.isEmpty()) result.append('\n').append(spillError);
                }
            }
            JSONObject quantumRam = preflight.optJSONObject("quantum_ram_probe");
            if (quantumRam != null && quantumRam.optBoolean("passed")) {
                result.append(String.format(Locale.getDefault(),
                        "\nPrueba RAM Éter: %d → %d · %d instrucciones x86",
                        quantumRam.optInt("loaded_value"), quantumRam.optInt("stored_value"),
                        quantumRam.optLong("guest_steps")));
            }
            for (String[] check : HARDWARE_CHECKS) {
                String key = check[0];
                String checkResult;
                if (!checks.has(key)) checkResult = "sin resultado";
                else if (checks.isNull(key)) {
                    checkResult = switch (key) {
                        case "qssd" -> "sin disco creado";
                        case "qssd_ata" -> "requiere un disco legible";
                        case "iso_boot" -> "sin ISO elegida";
                        default -> "sin resultado";
                    };
                } else if (checks.optBoolean(key)) checkResult = "aprobado";
                else if (key.startsWith("cpu_")) {
                    checkResult = "la CPU no anuncia esta capacidad";
                } else checkResult = "falló la comprobación";
                result.append('\n').append(check[1]).append(" · ").append(checkResult);
                String detail = errors == null ? "" : errors.optString(key);
                if (!detail.isEmpty()) result.append('\n').append(detail);
            }
            setHardwareResult(result.toString(), ready ? ACCENT : MUTED);
        } catch (JSONException error) {
            setHardwareResult("El resultado del equipo virtual no se pudo leer.", ERROR);
        }
        updateControls();
    }

    private void confirmStop() {
        Runnable stopAction = () -> {
            pending = true;
            updateControls();
            worker.stop();
        };
        if (fault || terminal) stopAction.run();
        else new AlertDialog.Builder(this)
                .setTitle("Apagar máquina")
                .setMessage("El QSSD conserva las escrituras ya confirmadas. Si el invitado está instalando, espera a que termine.")
                .setNegativeButton("Cancelar", null)
                .setPositiveButton("Apagar", (dialog, which) -> stopAction.run())
                .show();
    }

    @Override
    public void onState(String json, boolean active, boolean hasSession, boolean stopped, boolean detailed) {
        boolean wasFault = fault;
        boolean wasSession = session;
        boolean wasRunning = running;
        boolean wasTerminal = terminal;
        boolean wasPending = pending;
        boolean wasEvidenceEmpty = lastJson.isEmpty();
        pending = false;
        session = hasSession;
        running = active;
        terminal = stopped;
        if (detailed && !json.isEmpty()) lastJson = json;
        if (!session) {
            executionRate.reset();
            motorRate.reset();
            status.setTextColor(TEXT);
            status.setText("Máquina apagada");
            source.setText(BuildConfig.VERSION_NAME + " · Invitado x86-64");
            fault = false;
            telemetry.clear();
            sessionSummary.setVisibility(View.GONE);
        }
        if (!json.isEmpty()) {
            try {
                JSONObject diagnostic = new JSONObject(json);
                String code = diagnostic.optString("status");
                JSONObject boot = diagnostic.optJSONObject("boot_progress");
                if (session) {
                    switch (diagnostic.optString("source")) {
                        case "iso" -> source.setText("BIOS · CD/DVD");
                        case "qssd" -> source.setText("BIOS · QSSD");
                    }
                    fault = "fault".equals(code);
                    status.setTextColor(fault ? ERROR : TEXT);
                    String message = switch (code) {
                        case "waiting_key" -> "BIOS espera una tecla";
                        case "halted" -> terminal ? "La CPU invitada se detuvo"
                                : "La CPU invitada espera una interrupción";
                        case "powered_off" -> "El invitado se apagó";
                        case "fault" -> faultDetail(diagnostic);
                        default -> boot != null && boot.optBoolean("handed_off")
                                ? "Invitado en ejecución" : "Arranque en ejecución";
                    };
                    status.setText(!active && !fault && !terminal
                            ? "Máquina en pausa" : message);
                }
                updateBootProgress(diagnostic, boot);
            } catch (JSONException error) {
                stats.setText("Diagnóstico disponible para copiar.");
            }
        } else if (!session) {
            stats.setText("");
        }
        if (wasPending || session != wasSession || running != wasRunning || terminal != wasTerminal ||
                fault != wasFault || wasEvidenceEmpty != lastJson.isEmpty()) {
            updateControls();
        }
        if (session && !wasSession) restoreGuestFocus();
        if ((session && !wasSession) || (fault && !wasFault)) showBootStatus();
    }

    private static String faultDetail(JSONObject diagnostic) {
        JSONObject cpuFault = diagnostic.optJSONObject("cpu_fault");
        if (cpuFault != null) {
            String message = cpuFault.optString("message");
            if (!message.isEmpty()) return "CPU " + cpuFault.optInt("apic_id") + " · " + message;
        }
        JSONObject machine = diagnostic.optJSONObject("machine");
        String message = (machine == null ? diagnostic : machine).optString("message");
        return message.isEmpty() ? "Arranque detenido por un fallo" : message;
    }

    private void updateBootProgress(JSONObject diagnostic, JSONObject boot) {
        JSONObject machine = diagnostic.optJSONObject("machine");
        if (machine == null) machine = diagnostic;
        JSONObject ata = diagnostic.optJSONObject("ata");
        JSONObject atapi = diagnostic.optJSONObject("atapi");
        JSONObject irq = diagnostic.optJSONObject("irq");
        JSONObject volume = diagnostic.optJSONObject("disk");
        JSONObject ether = diagnostic.optJSONObject("ether_next");
        JSONObject ram = diagnostic.optJSONObject("ram_profile");
        long steps = machine.optLong("instructions", -1);
        long sessionSteps = boot == null ? steps : boot.optLong("session_steps", steps);
        JSONObject workerTiming = diagnostic.optJSONObject("worker_timing");
        long sampleUs = workerTiming == null ? -1 : workerTiming.optLong("sample_elapsed_us", -1);
        boolean executing = session && running && "running".equals(diagnostic.optString("status"));
        long stepsPerSecond = executionRate.sample(executing, sessionSteps, sampleUs);
        long motorUs = workerTiming == null ? -1 : workerTiming.optLong("step_us", -1);
        long motorStepsPerSecond = motorRate.sample(executing, sessionSteps, motorUs);
        if (session) {
            liveMetrics.signal(diagnostic);
            telemetry.update(diagnostic, machine, modeName(machine.optString("cpu_mode")),
                    sessionSteps, stepsPerSecond, motorStepsPerSecond);
            sessionSummary.setText(VmTelemetry.summary(diagnostic, machine, sessionSteps));
            sessionSummary.setVisibility(View.VISIBLE);
        }
        long reads = machine.optLong("disk_reads");
        long readSectors = ata == null ? 0 : ata.optLong("read_sectors");
        long identifyTransfers = ata == null ? 0 : ata.optLong("identify_data_transfers");
        long batchedElements = ether == null ? 0 : ether.optLong("boot_rep_elements");
        long shiftLoopSteps = ether == null ? 0 : ether.optLong("boot_shift_loop_steps");
        long zeroLoopSteps = ether == null ? 0 : ether.optLong("boot_zero_loop_steps");
        long counterLoopSteps = ether == null ? 0 : ether.optLong("boot_counter_loop_steps");
        long commits = volume == null ? 0 : volume.optLong("commits");
        boolean handedOff = boot == null ? steps > 0 : boot.optBoolean("handed_off");
        StringBuilder progress = new StringBuilder(handedOff ? "BIOS entregó el control al invitado"
                : "BIOS preparando el arranque");
        progress.append("\nModo ").append(modeName(machine.optString("cpu_mode")));
        progress.append(String.format(Locale.getDefault(), "\n%,d pasos x86 en esta sesión", sessionSteps));
        JSONObject engine = diagnostic.optJSONObject("ether_execution");
        if (engine != null) {
            progress.append(String.format(Locale.getDefault(), "\nÉter · objetivo %.1f THz virtuales",
                    engine.optLong("target_virtual_cycles_per_second") / 1e12));
        }
        if (stepsPerSecond >= 0) progress.append(String.format(Locale.getDefault(),
                " · %,d pasos x86/s medidos", stepsPerSecond));
        if (workerTiming != null && workerTiming.optLong("max_steps_per_slice") > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nTanda de hasta %,d pasos · Guardado cada %d s",
                    workerTiming.optLong("max_steps_per_slice"),
                    workerTiming.optLong("diagnostic_save_interval_ms") / 1000));
        }
        if (boot != null && boot.optLong("checkpoint_interval_steps") > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nPunto de control cada %,d pasos del núcleo de arranque",
                    boot.optLong("checkpoint_interval_steps")));
        }
        JSONObject topology = diagnostic.optJSONObject("cpu_topology");
        if (topology != null) {
            progress.append(String.format(Locale.getDefault(),
                    "\nCPU · %d núcleos virtuales · %d/%d vCPU iniciadas · %d trabajadores asignados",
                    topology.optInt("cores"), topology.optInt("started"),
                    topology.optInt("logical_processors"), topology.optInt("host_workers")));
            if (topology.has("phase_projected_steps")) {
                progress.append(String.format(Locale.getDefault(),
                        "\nVectores · %,d pasos proyectados · %d vCPU · %,d bloques encadenados",
                        topology.optLong("phase_projected_steps"), topology.optInt("vcpus_with_projected_steps"),
                        topology.optLong("parallel_register_continuations")));
            }
        }
        JSONObject coverage = diagnostic.optJSONObject("ether_coverage");
        if (coverage != null && coverage.optLong("interpreted_steps", -1) >= 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nPasos de la fase · %,d traducidos · %,d en bloque · %,d instrucción a instrucción",
                    coverage.optLong("translated_steps"), coverage.optLong("batched_steps"),
                    coverage.optLong("interpreted_steps")));
        }
        if (ram != null) {
            progress.append(String.format(Locale.getDefault(), "\nRAM del invitado · %,d MiB",
                    ram.optLong("usable_bytes") / MIB));
            progress.append(String.format(Locale.getDefault(), "\nDatos de RAM residentes · %,d / %,d MiB",
                    ram.optLong("resident_bytes") / MIB, ram.optLong("backing_budget_bytes") / MIB));
            if ("file".equals(ram.optString("spill_mode"))) {
                progress.append(String.format(Locale.getDefault(), " · Temporal: %,d MiB",
                        ram.optLong("spilled_bytes") / MIB));
            } else if ("unavailable".equals(ram.optString("spill_mode"))) {
                progress.append("\nRespaldo temporal de RAM no disponible.");
            }
        }
        if (batchedElements > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nEther · %,d elementos de copia o relleno en bloque", batchedElements));
        }
        if (shiftLoopSteps > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nEther · %,d instrucciones de bucles de bits en bloque", shiftLoopSteps));
        }
        if (zeroLoopSteps > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nEther · %,d instrucciones de borrado de RAM en bloque", zeroLoopSteps));
        }
        if (counterLoopSteps > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nEther · %,d pasos equivalentes de cuenta regresiva", counterLoopSteps));
        }
        if (boot != null && boot.optLong("mode_changes") > 0) {
            progress.append("\nÚltimo cambio: ").append(modeName(boot.optString("last_mode_from")))
                    .append(" → ").append(modeName(boot.optString("last_mode_to")));
        }
        long restarts = boot == null ? 0 : boot.optLong("restarts");
        if (restarts > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nReinicios automáticos · %,d · %,d pasos x86 desde el último", restarts,
                    topology == null ? steps : topology.optLong("phase_x86_steps", steps)));
        }
        if ("iso".equals(diagnostic.optString("source"))) {
            progress.append(String.format(Locale.getDefault(), "\nLecturas de la ISO por BIOS: %,d", reads));
            if (atapi != null && atapi.optLong("packets") > 0) {
                progress.append(String.format(Locale.getDefault(), "\nSectores del CD leídos por ATAPI: %,d",
                        atapi.optLong("read_sectors")));
            }
        } else if (volume != null) {
            progress.append(String.format(Locale.getDefault(), "\nLecturas del disco por BIOS: %,d", reads));
        }
        if (volume != null) {
            if (identifyTransfers > 0) {
                progress.append("\nEl invitado leyó la identificación ATA del QSSD.");
            }
            progress.append(String.format(Locale.getDefault(),
                    "\nSectores leídos por ATA: %,d · Operaciones guardadas en QSSD: %,d", readSectors, commits));
        }
        String detail = "fault".equals(diagnostic.optString("status")) ?
                faultDetail(diagnostic) : machine.optString("message");
        if (irq != null && irq.optBoolean("halted_with_if")) {
            progress.append(String.format(Locale.getDefault(),
                    "\nInterrupciones entregadas: %,d · Temporizador APIC: %,d",
                    irq.optLong("delivered"), irq.optLong("lapic_timer_count")));
        }
        if ("fault".equals(diagnostic.optString("status")) && !detail.isEmpty()) {
            progress.append("\nÚltimo bloqueo: ").append(detail);
        } else if (session && handedOff && blankTextFrame) {
            progress.append("\nLa pantalla VGA todavía está en blanco.");
        }
        String ip = machine.optString("rip_hex");
        if (!ip.isEmpty()) {
            progress.append("\nDirección de CPU · 0x").append(ip);
        }
        stats.setText(progress);
    }

    private static String modeName(String mode) {
        return switch (mode) {
            case "real16" -> "real de 16 bits";
            case "protected16" -> "protegido de 16 bits";
            case "protected32" -> "protegido de 32 bits";
            case "compat16" -> "de compatibilidad de 16 bits";
            case "compat32" -> "de compatibilidad de 32 bits";
            case "long64" -> "de 64 bits";
            default -> mode.isEmpty() ? "sin datos" : mode;
        };
    }

    private void showBootStatus() {
        if(!status.isShown())return;
        status.post(() -> status.requestRectangleOnScreen(
                new Rect(0, 0, status.getWidth(), status.getHeight())));
    }

    @Override
    public void onFrame(VmWorker.Frame frame) {
        boolean blank = frame.text != null;
        if (frame.text != null) {
            for (int cell = 0; cell + 1 < frame.text.length; cell += 2) {
                int character = frame.text[cell] & 0xff;
                int attribute = frame.text[cell + 1] & 0xff;
                if ((attribute & 0x70) != 0 || (character != 0 && character != 32 && (attribute & 0x0f) != 0)) {
                    blank = false;
                    break;
                }
            }
        }
        blankTextFrame = blank;
        monitor.show(frame);
        if(page==1)updateDisplayValues();
    }

    @Override
    public void onError(String message, boolean hasSession) {
        pending = false;
        if (checkingHardware) {
            checkingHardware = false;
            setHardwareResult("No se pudo comprobar el equipo virtual.\n" + message, ERROR);
            updateControls();
            return;
        }
        session = hasSession;
        running = false;
        fault = true;
        status.setTextColor(ERROR);
        status.setText("Máquina detenida");
        stats.setText("Último bloqueo: " + message);
        if (!session) {
            telemetry.clear();
            sessionSummary.setVisibility(View.GONE);
        }
        lastJson = "{\"schema\":\"reverse-windows-android-error-v1\",\"status\":\"fault\",\"message\":"
                + JSONObject.quote(message) + "}";
        updateControls();
        showBootStatus();
    }

    @Override
    public void onDiskCreated(boolean data) {
        pending = false;
        status.setText(data ? "QSSD de datos listo" : "QSSD listo");
        status.setTextColor(TEXT);
        stats.setText(data ? "Inicializa el disco de datos como GPT en Windows."
                : "El disco virtual está vacío.");
        clearHardwareResult();
        updateControls();
    }

    private void updateControls() {
        if (session && running && !fault && !terminal)
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        liveMetrics.running(session&&running&&!fault&&!terminal);
        console.state(session||pending,fault,status.getText());
        boolean diskReady = disk.isFile();
        boolean dataDiskReady = dataDisk.isFile();
        media.setText(iso == null ? "Selecciona tu ISO o RISO local" : "CD/DVD · " + isoName);
        String bootStorage = diskReady ? "QSSD de arranque · 64 GiB lógicos"
                : "QSSD · se crea al arrancar la ISO";
        storage.setText(dataDiskReady ? bootStorage + "\nQSSD de datos · 8 PiB lógicos" : bootStorage);
        boolean setup = !session && !pending;
        setupPanel.setVisibility(session ? View.GONE : View.VISIBLE);
        ramProfile.setText(primeRam ? "RAM del invitado · 64 GiB" : "RAM del invitado · Automática");
        ramProfile.setEnabled(setup);
        selectIso.setEnabled(setup);
        ejectIso.setEnabled(setup && iso != null);
        createDisk.setVisibility(diskReady ? View.GONE : View.VISIBLE);
        createDisk.setEnabled(setup);
        createDataDisk.setVisibility(dataDiskReady ? View.GONE : View.VISIBLE);
        createDataDisk.setEnabled(setup);
        bootIso.setEnabled(setup && iso != null);
        bootDisk.setEnabled(setup && diskReady);
        biosTest.setEnabled(setup);
        hardwareCheck.setEnabled(setup);
        controls.setVisibility(session ? View.VISIBLE : View.GONE);
        experimentalClock.setEnabled(session && !pending && !fault && !terminal);
        pause.setText(running ? "Pausar" : "Reanudar");
        pause.setEnabled(session && !pending && !fault && !terminal);
        stop.setEnabled(session && !pending);
        evidence.setVisibility(lastJson.isEmpty() ? View.GONE : View.VISIBLE);
        evidence.setEnabled(!requestingDiagnostic);
    }

    private void showDiagnostic() {
        if (session && !requestingDiagnostic) {
            requestingDiagnostic = true;
            evidence.setEnabled(false);
            worker.requestDiagnostic(json -> {
                requestingDiagnostic = false;
                evidence.setEnabled(true);
                if (!json.isEmpty()) lastJson = json;
                showDiagnosticDialog(lastJson);
            });
        } else if (!requestingDiagnostic) {
            showDiagnosticDialog(lastJson);
        }
    }

    private void showDiagnosticDialog(String json) {
        if (json.isEmpty()) return;
        boolean timingEnabled = false;
        boolean timingKnown = false;
        try {
            JSONObject ram = new JSONObject(json).optJSONObject("ram_profile");
            JSONObject pressure = ram == null ? null : ram.optJSONObject("access_pressure");
            timingKnown = pressure != null && pressure.opt("backing_timing_enabled") instanceof Boolean;
            timingEnabled = timingKnown && pressure.optBoolean("backing_timing_enabled", false);
        } catch (org.json.JSONException ignored) {
            // An old snapshot cannot report a measurement mode it did not expose.
        }
        final boolean enableTiming = !timingEnabled;
        final boolean canChangeTiming = session && !pending && timingKnown;
        LinearLayout title = column();
        title.setPadding(dp(24), dp(20), dp(24), dp(8));
        add(title, label("Último diagnóstico", 20, TEXT), 0);
        String timingState = timingKnown ? (timingEnabled ? "activos" : "apagados") : "sin dato";
        add(title, label("Cronómetros de lectura del respaldo: " + timingState
                + " en este diagnóstico. Medir añade coste y puede reducir la velocidad.", 13, MUTED), 8);
        String[] choices = canChangeTiming
                ? new String[]{"Copiar texto", "Guardar archivo JSON", "Guardar archivo TXT",
                        enableTiming ? "Activar cronómetros de lectura del respaldo"
                                : "Desactivar cronómetros de lectura del respaldo"}
                : new String[]{"Copiar texto", "Guardar archivo JSON", "Guardar archivo TXT"};
        new AlertDialog.Builder(this)
                .setCustomTitle(title)
                .setItems(choices,
                        (dialog, choice) -> {
                    if (choice == 0) copyEvidence(json);
                    else if (choice < 3) saveEvidence(json, choice == 2);
                    else changeRamTiming(enableTiming);
                })
                .setNegativeButton("Cerrar", null)
                .show();
    }

    private void changeRamTiming(boolean enabled) {
        if (!session || pending || requestingDiagnostic) return;
        requestingDiagnostic = true;
        evidence.setEnabled(false);
        worker.setBackingReadTiming(enabled, json -> {
            requestingDiagnostic = false;
            updateControls();
            if (json.isEmpty()) {
                Toast.makeText(this, "No se pudo cambiar la medición de RAM de esta sesión.",
                        Toast.LENGTH_LONG).show();
                return;
            }
            lastJson = json;
            showDiagnosticDialog(json);
        });
    }

    private void copyEvidence(String json) {
        ClipboardManager clipboard = getSystemService(ClipboardManager.class);
        try {
            clipboard.setPrimaryClip(ClipData.newPlainText("REVERSE Windows", json));
            if (Build.VERSION.SDK_INT < 33)
                Toast.makeText(this, "Diagnóstico copiado", Toast.LENGTH_SHORT).show();
        } catch (RuntimeException error) {
            Toast.makeText(this, "No se pudo copiar el diagnóstico.", Toast.LENGTH_LONG).show();
        }
    }

    private void saveEvidence(String json, boolean plainText) {
        diagnosticToExport = json;
        Intent picker = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType(plainText ? "text/plain" : "application/json");
        picker.putExtra(Intent.EXTRA_TITLE, plainText ? "reverse-windows-diagnostico.txt"
                : "reverse-windows-diagnostico.json");
        try {
            startActivityForResult(picker, DIAGNOSTIC_PICKER);
        } catch (android.content.ActivityNotFoundException error) {
            diagnosticToExport = "";
            Toast.makeText(this, "Android no encontró dónde guardar el archivo.",
                    Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void onSavedDiagnostic(String json) {
        if (json.isEmpty()) return;
        lastJson = json;
        updateControls();
    }

    @Override
    public void onDiagnosticExported(boolean saved) {
        Toast.makeText(this, saved ? "Diagnóstico guardado" : "No se pudo guardar el diagnóstico.",
                Toast.LENGTH_LONG).show();
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putString("diagnosticToExport", diagnosticToExport);
    }

    private boolean canQueueGuestInput() {
        // The worker, which also handles foreground changes, owns the
        // running flag. The status broadcast can arrive after the first key.
        return session && !pending && !fault && !terminal;
    }

    private void sendGuestKey(int ascii, int scan) {
        if (canQueueGuestInput()) {
            worker.key(ascii, scan, true);
            worker.key(ascii, scan, false);
        }
    }

    @Override
    public boolean onKey(KeyEvent event) {
        if (!canQueueGuestInput()) return false;
        if(event.getAction()==KeyEvent.ACTION_DOWN)liveMetrics.input();
        int ascii = GuestKeys.ascii(event);
        int scan = GuestKeys.scan(event.getKeyCode(), ascii);
        if (ascii == 0 && scan == 0) return false;
        if (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP) {
            worker.key(ascii, scan, event.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return false;
    }

    @Override
    public void onText(CharSequence text) {
        if (!canQueueGuestInput()) return;
        liveMetrics.input();
        for (int index = 0; index < Math.min(text.length(), 256); index++) {
            char key = text.charAt(index);
            int scan = GuestKeys.scanAscii(key);
            if (scan == 0) continue;
            boolean shift = GuestKeys.needsShift(key);
            if (shift) worker.key(0, 0x2a, true);
            sendGuestKey(key == '\n' ? 13 : key, scan);
            if (shift) worker.key(0, 0x2a, false);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (monitor != null && monitor.hasFocus() && onKey(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) restoreGuestFocus();
    }

    private void restoreGuestFocus() {
        if (monitor != null && monitor.isShown() && canQueueGuestInput()) monitor.requestFocus();
    }

    @Override
    protected void onStart() {
        super.onStart();
        worker.setForeground(true);
        liveMetrics.start();
    }

    @Override
    protected void onStop() {
        console.release();
        liveMetrics.stop();
        worker.setForeground(false);
        super.onStop();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (worker == null) return;
        if (level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND)
            worker.trimRam(true);
        else if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW &&
                 level < ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN)
            worker.trimRam(false);
    }

    @Override
    protected void onDestroy() {
        worker.release();
        super.onDestroy();
    }

    private LinearLayout card(LinearLayout parent, int spacing) {
        LinearLayout card = column();
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackground(panel(PANEL, 15, BORDER));
        add(parent, card, spacing);
        return card;
    }

    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    LinearLayout row(LinearLayout parent) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        parent.addView(layout, new LinearLayout.LayoutParams(-1, -2));
        return layout;
    }

    TextView label(String value, int size, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    Button action(String value, boolean primary, Runnable onClick) {
        Button button = new Button(this);
        button.setAllCaps(false);
        button.setText(value);
        button.setTextSize(13);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(dp(47));
        button.setMinimumHeight(dp(47));
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setPadding(dp(8), dp(5), dp(8), dp(5));
        button.setStateListAnimator(null);
        int enabled = primary ? 0xff092019 : TEXT;
        int disabled = primary ? 0xff798986 : 0xff7e8ea2;
        button.setTextColor(new ColorStateList(
                new int[][]{{-android.R.attr.state_enabled}, {}}, new int[]{disabled, enabled}));
        int background = primary ? 0xd9baf6e5 : 0x7043545c;
        android.graphics.drawable.StateListDrawable fills = new android.graphics.drawable.StateListDrawable();
        fills.addState(new int[]{-android.R.attr.state_enabled}, panel(0x531f2c31, 9, BORDER));
        fills.addState(new int[]{android.R.attr.state_selected}, panel(0x468be2c6, 9, 0x88baf6e5));
        fills.addState(new int[]{}, panel(background, 9, primary ? ACCENT : BORDER));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33baf6e5), fills, null));
        button.setOnClickListener(v -> onClick.run());
        return button;
    }

    GradientDrawable panel(int color, int radius, int border) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(color);
        background.setCornerRadius(dp(radius));
        background.setStroke(dp(1), border);
        return background;
    }

    private void pair(LinearLayout row, Button left, Button right) {
        weighted(row, left, 0);
        weighted(row, right, 7);
    }

    void weighted(LinearLayout row, Button view, int spacing) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, 1);
        params.leftMargin = dp(spacing);
        row.addView(view, params);
    }

    void add(LinearLayout parent, View view, int spacing) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(spacing);
        parent.addView(view, params);
    }

    int dp(int amount) {
        return Math.round(getResources().getDisplayMetrics().density * amount);
    }
}
