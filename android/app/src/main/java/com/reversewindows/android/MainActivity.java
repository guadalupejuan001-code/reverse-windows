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
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.util.Locale;

public final class MainActivity extends Activity implements VmWorker.Listener, GuestMonitor.Input {
    private static final int ISO_PICKER = 21;
    private static final int DIAGNOSTIC_PICKER = 22;
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
    private ScrollView scroll;
    private ScrollView traceScroll;
    private LinearLayout workbench;
    private LinearLayout viewer;
    private FrameLayout guestSurface;
    private VmTelemetry telemetry;
    private TextView sessionSummary;
    private Button screenTab;
    private Button traceTab;
    private Button expandMonitor;
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
    private Button stop;
    private Button evidence;
    private Button hardwareCheck;
    private Button ramProfile;
    private TextView hardwareResult;
    private LinearLayout controls;
    private LinearLayout primaryActions;
    private LinearLayout guestKeys;
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
        scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[]{0xff2a404a, BG, 0xff204038}));
        scroll.setClipToPadding(false);
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(
                        WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                android.graphics.Insets keyboard = insets.getInsets(WindowInsets.Type.ime());
                view.setPadding(safe.left, safe.top, safe.right, Math.max(safe.bottom, keyboard.bottom));
            }
            return insets;
        });
        LinearLayout body = column();
        body.setPadding(dp(12), dp(8), dp(12), dp(12));
        scroll.addView(body);

        LinearLayout header = row(body);
        header.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout identity = column();
        header.addView(identity, new LinearLayout.LayoutParams(0, -2, 1));
        TextView title = label("REVERSE Windows", 20, TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        identity.addView(title);
        source = label(BuildConfig.VERSION_NAME + " · Invitado x86-64", 12, ACCENT);
        source.setTypeface(Typeface.MONOSPACE);
        add(identity, source, 2);
        evidence = action("Diagnóstico", false, this::showDiagnostic);
        evidence.setId(R.id.vm_copy_diagnostic);
        LinearLayout.LayoutParams evidenceParams = new LinearLayout.LayoutParams(-2, -2);
        evidenceParams.leftMargin = dp(8);
        header.addView(evidence, evidenceParams);

        status = label("Máquina apagada", 14, TEXT);
        status.setId(R.id.vm_status);
        status.setTypeface(Typeface.DEFAULT_BOLD);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        add(body, status, 5);
        setupPanel = card(body, 10);
        LinearLayout display = card(body, 9);
        workbench = row(display);
        viewer = column();
        workbench.addView(viewer);

        LinearLayout viewTabs = row(viewer);
        screenTab = action("Pantalla", false, () -> showTrace(false));
        traceTab = action("Registro", false, () -> showTrace(true));
        expandMonitor = action("Ampliar", false, this::toggleMonitorSize);
        weighted(viewTabs, screenTab, 0);
        weighted(viewTabs, traceTab, 6);
        weighted(viewTabs, expandMonitor, 6);

        guestSurface = new FrameLayout(this);
        guestSurface.setBackground(panel(0x9910161a, 8, BORDER));
        guestSurface.setClipToOutline(true);
        add(viewer, guestSurface, 6);
        monitor = new GuestMonitor(this, this);
        guestSurface.addView(monitor, new FrameLayout.LayoutParams(-1, -1));

        traceScroll = new ScrollView(this);
        traceScroll.setFillViewport(true);
        stats = label("", 13, MUTED);
        stats.setTypeface(Typeface.MONOSPACE);
        stats.setTextIsSelectable(true);
        stats.setPadding(dp(10), dp(9), dp(10), dp(9));
        traceScroll.addView(stats);
        guestSurface.addView(traceScroll, new FrameLayout.LayoutParams(-1, -1));
        showTrace(false);

        telemetry = new VmTelemetry(this);
        workbench.addView(telemetry);

        sessionSummary = label("", 11, MUTED);
        sessionSummary.setTypeface(Typeface.MONOSPACE);
        sessionSummary.setVisibility(View.GONE);
        add(display, sessionSummary, 6);

        controls = column();
        add(display, controls, 6);
        primaryActions = row(controls);
        LinearLayout sessionRow = row(primaryActions);
        pause = action("Pausar", false, () -> worker.setRunning(!running));
        stop = action("Apagar", false, this::confirmStop);
        pair(sessionRow, pause, stop);
        LinearLayout inputRow = row(primaryActions);
        pair(inputRow, action("Teclado", false, () -> {
            showTrace(false);
            monitor.showKeyboard();
            scroll.postDelayed(() -> scroll.smoothScrollTo(0, 0), 220);
        }), action("Enter ↵", false, () -> sendGuestKey(13, 0x1c)));
        guestKeys = row(controls);
        ((LinearLayout.LayoutParams) guestKeys.getLayoutParams()).topMargin = dp(5);
        LinearLayout arrows = row(guestKeys);
        String[] arrowsText = {"←", "↑", "↓", "→"};
        String[] arrowsDescription = {"Izquierda", "Arriba", "Abajo", "Derecha"};
        int[] arrowsScan = {0xe04b, 0xe048, 0xe050, 0xe04d};
        for (int i = 0; i < arrowsText.length; i++) {
            int scan = arrowsScan[i];
            Button key = action(arrowsText[i], false, () -> sendGuestKey(0, scan));
            key.setContentDescription(arrowsDescription[i]);
            weighted(arrows, key, i == 0 ? 0 : 6);
        }
        LinearLayout extraKeys = row(guestKeys);
        weighted(extraKeys, action("Esc", false, () -> sendGuestKey(27, 0x01)), 0);
        weighted(extraKeys, action("Tab", false, () -> sendGuestKey(9, 0x0f)), 6);
        weighted(extraKeys, action("F8", false, () -> sendGuestKey(0, 0x42)), 6);
        weighted(extraKeys, action("F10", false, () -> sendGuestKey(0, 0x44)), 6);

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
        layoutWorkbench(getResources().getConfiguration());
        setContentView(scroll);
    }

    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        layoutWorkbench(configuration);
    }

    private void layoutWorkbench(Configuration configuration) {
        int width = configuration.screenWidthDp;
        boolean landscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
                && width >= 600;
        workbench.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        viewer.setLayoutParams(new LinearLayout.LayoutParams(landscape ? 0 : -1, -2, landscape ? 1 : 0));
        LinearLayout.LayoutParams sidebar = new LinearLayout.LayoutParams(
                landscape ? dp(Math.max(228, Math.min(320, width * 34 / 100))) : -1, -2);
        sidebar.leftMargin = landscape ? dp(9) : 0;
        sidebar.topMargin = landscape ? 0 : dp(9);
        telemetry.setLayoutParams(sidebar);
        int height = landscape ? configuration.screenHeightDp - 197 : (width - 48) * 5 / 8;
        LinearLayout.LayoutParams screen = (LinearLayout.LayoutParams) guestSurface.getLayoutParams();
        screen.height = dp(Math.max(145, Math.min(380, height)));
        guestSurface.setLayoutParams(screen);
        layoutActionBand(primaryActions, landscape);
        layoutActionBand(guestKeys, landscape);
    }

    private void layoutActionBand(LinearLayout band, boolean landscape) {
        band.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        for (int i = 0; i < band.getChildCount(); i++) {
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                    landscape ? 0 : -1, -2, landscape ? 1 : 0);
            params.leftMargin = landscape && i > 0 ? dp(7) : 0;
            params.topMargin = !landscape && i > 0 ? dp(5) : 0;
            band.getChildAt(i).setLayoutParams(params);
        }
    }

    private void showTrace(boolean visible) {
        monitor.setVisibility(visible ? View.GONE : View.VISIBLE);
        traceScroll.setVisibility(visible ? View.VISIBLE : View.GONE);
        screenTab.setSelected(!visible);
        traceTab.setSelected(visible);
        if (!visible && session) monitor.requestFocus();
    }

    private void toggleMonitorSize() {
        boolean expanded = !expandMonitor.isSelected();
        expandMonitor.setSelected(expanded);
        expandMonitor.setText(expanded ? "Restaurar" : "Ampliar");
        telemetry.setVisibility(expanded ? View.GONE : View.VISIBLE);
        if (!expanded) telemetry.render();
    }

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
        pending = true;
        fault = false;
        terminal = false;
        blankTextFrame = false;
        executionRate.reset();
        monitor.reset();
        showTrace(false);
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
        if (fault && !wasFault) showTrace(true);
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
        long stepUs = workerTiming == null ? -1 : workerTiming.optLong("step_us", -1);
        long stepsPerSecond = executionRate.sample(session && running &&
                "running".equals(diagnostic.optString("status")), sessionSteps, stepUs);
        if (session) {
            telemetry.update(diagnostic, machine, modeName(machine.optString("cpu_mode")),
                    sessionSteps, stepsPerSecond);
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
        if (stepsPerSecond >= 0) {
            progress.append(String.format(Locale.getDefault(), " · %,d pasos/s de ejecución", stepsPerSecond));
        }
        if (workerTiming != null && workerTiming.optInt("max_steps_per_slice") > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nTanda de hasta %,d pasos · Guardado cada %d s",
                    workerTiming.optInt("max_steps_per_slice"),
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
        showTrace(true);
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
        new AlertDialog.Builder(this)
                .setTitle("Último diagnóstico")
                .setItems(new String[]{"Copiar texto", "Guardar archivo JSON", "Guardar archivo TXT"},
                        (dialog, choice) -> {
                    if (choice == 0) copyEvidence(json);
                    else saveEvidence(json, choice == 2);
                })
                .setNegativeButton("Cerrar", null)
                .show();
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
    }

    @Override
    protected void onStop() {
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
