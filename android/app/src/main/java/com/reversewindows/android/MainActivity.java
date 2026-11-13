package com.reversewindows.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
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
    private static final int BG = 0xff0c111a;
    private static final int PANEL = 0xff151e2a;
    private static final int BORDER = 0xff283547;
    private static final int TEXT = 0xffeef4fc;
    private static final int MUTED = 0xffa4b3c7;
    private static final int ACCENT = 0xff77e5cb;
    private static final int ERROR = 0xffffa9a4;
    private static final String[][] HARDWARE_CHECKS = {
            {"cpu_x64", "CPU x64"}, {"cpu_mmx", "MMX"}, {"cpu_sse", "SSE"},
            {"cpu_sse2", "SSE2"}, {"cpu_x87", "x87"},
            {"cpu_pae", "PAE"}, {"cpu_nx", "NX"}, {"cpu_cx16", "CMPXCHG16B"},
            {"cpu_lahf_sahf", "LAHF/SAHF"}, {"cpu_prefetchw", "PREFETCHW"}, {"cpu_fxsr", "FXSR"},
            {"ram", "RAM"}, {"pci_ide", "Controlador PCI IDE"}, {"vga", "Video VGA"},
            {"iso_boot", "Arranque desde ISO"}, {"qssd", "Archivo QSSD"},
            {"qssd_ata", "Capacidad QSSD por ATA"},
    };

    private File disk;
    private SharedPreferences prefs;
    private Uri iso;
    private String isoName;
    private VmWorker worker;
    private GuestMonitor monitor;
    private ScrollView scroll;
    private TextView media;
    private TextView storage;
    private TextView status;
    private TextView stats;
    private TextView source;
    private Button selectIso;
    private Button ejectIso;
    private Button createDisk;
    private Button bootIso;
    private Button bootDisk;
    private Button biosTest;
    private Button pause;
    private Button stop;
    private Button evidence;
    private Button hardwareCheck;
    private TextView hardwareResult;
    private LinearLayout controls;
    private boolean pending;
    private boolean session;
    private boolean running;
    private boolean fault;
    private boolean terminal;
    private boolean checkingHardware;
    private boolean blankTextFrame;
    private String lastJson = "";

    @Override
    public void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        disk = new File(getFilesDir(), "windows.rqvd");
        prefs = getSharedPreferences("vm", MODE_PRIVATE);
        String savedIso = prefs.getString("iso", "");
        if (!savedIso.isEmpty()) {
            iso = Uri.parse(savedIso);
            isoName = prefs.getString("iso_name", "ISO");
        }
        worker = new VmWorker(this, disk, this);
        buildScreen();
        updateControls();
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra(
                "com.reversewindows.android.SELF_TEST", false)) {
            start(NativeMachine.BOOT_SELF_TEST, true);
        }
    }

    private void buildScreen() {
        scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
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
        body.setPadding(dp(18), dp(14), dp(18), dp(20));
        scroll.addView(body);

        TextView title = label("REVERSE Windows", 27, TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        body.addView(title);
        source = label("BIOS · x86-64", 12, ACCENT);
        source.setTypeface(Typeface.MONOSPACE);
        source.setLetterSpacing(0.07f);
        add(body, source, 4);

        LinearLayout display = card(body, 14);
        status = label("Máquina apagada", 16, TEXT);
        status.setId(R.id.vm_status);
        status.setTypeface(Typeface.DEFAULT_BOLD);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        display.addView(status);
        stats = label("", 13, MUTED);
        stats.setTextIsSelectable(true);
        add(display, stats, 5);
        evidence = action("Copiar diagnóstico", false, this::copyEvidence);
        evidence.setId(R.id.vm_copy_diagnostic);
        add(display, evidence, 8);

        monitor = new GuestMonitor(this, this);
        monitor.setBackground(panel(0xff080b10, 9, BORDER));
        monitor.setClipToOutline(true);
        int availableWidth = getResources().getConfiguration().screenWidthDp;
        int displayHeight = Math.max(200, Math.min(370, (availableWidth - 62) * 5 / 8));
        LinearLayout.LayoutParams monitorParams = new LinearLayout.LayoutParams(-1, dp(displayHeight));
        monitorParams.topMargin = dp(10);
        display.addView(monitor, monitorParams);

        controls = column();
        add(display, controls, 9);
        LinearLayout sessionRow = row(controls);
        pause = action("Pausar", false, () -> worker.setRunning(!running));
        stop = action("Apagar", false, this::confirmStop);
        pair(sessionRow, pause, stop);
        LinearLayout inputRow = row(controls);
        LinearLayout.LayoutParams inputParams = (LinearLayout.LayoutParams) inputRow.getLayoutParams();
        inputParams.topMargin = dp(6);
        pair(inputRow, action("Teclado", false, () -> {
            monitor.showKeyboard();
            scroll.postDelayed(() -> scroll.smoothScrollTo(0, 0), 220);
        }), action("Enter ↵", false, () -> sendGuestKey(13, 0x1c)));
        LinearLayout arrows = row(controls);
        ((LinearLayout.LayoutParams) arrows.getLayoutParams()).topMargin = dp(5);
        String[] arrowsText = {"←", "↑", "↓", "→"};
        String[] arrowsDescription = {"Izquierda", "Arriba", "Abajo", "Derecha"};
        int[] arrowsScan = {0xe04b, 0xe048, 0xe050, 0xe04d};
        for (int i = 0; i < arrowsText.length; i++) {
            int scan = arrowsScan[i];
            Button key = action(arrowsText[i], false, () -> sendGuestKey(0, scan));
            key.setContentDescription(arrowsDescription[i]);
            weighted(arrows, key, i == 0 ? 0 : 6);
        }
        LinearLayout extraKeys = row(controls);
        ((LinearLayout.LayoutParams) extraKeys.getLayoutParams()).topMargin = dp(5);
        weighted(extraKeys, action("Esc", false, () -> sendGuestKey(27, 0x01)), 0);
        weighted(extraKeys, action("Tab", false, () -> sendGuestKey(9, 0x0f)), 6);
        weighted(extraKeys, action("F8", false, () -> sendGuestKey(0, 0x42)), 6);
        weighted(extraKeys, action("F10", false, () -> sendGuestKey(0, 0x44)), 6);

        LinearLayout setup = card(body, 12);
        TextView bootTitle = label("Medios de arranque", 16, TEXT);
        bootTitle.setTypeface(Typeface.DEFAULT_BOLD);
        setup.addView(bootTitle);
        media = label("", 13, MUTED);
        media.setSingleLine(true);
        media.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        add(setup, media, 10);
        LinearLayout mediaRow = row(setup);
        ((LinearLayout.LayoutParams) mediaRow.getLayoutParams()).topMargin = dp(5);
        selectIso = action("Elegir ISO", false, this::pickIso);
        selectIso.setId(R.id.vm_select_iso);
        ejectIso = action("Expulsar ISO", false, this::clearIso);
        pair(mediaRow, selectIso, ejectIso);

        storage = label("", 13, MUTED);
        add(setup, storage, 9);
        createDisk = action("Crear QSSD de 64 GiB", false, this::prepareDisk);
        createDisk.setId(R.id.vm_create_disk);
        add(setup, createDisk, 5);

        bootIso = action("Arrancar desde ISO", true, () -> start(NativeMachine.BOOT_ISO, false));
        bootIso.setId(R.id.vm_boot_iso);
        add(setup, bootIso, 12);
        LinearLayout bootRow = row(setup);
        ((LinearLayout.LayoutParams) bootRow.getLayoutParams()).topMargin = dp(6);
        bootDisk = action("Arrancar QSSD", false, () -> start(NativeMachine.BOOT_QSSD, false));
        bootDisk.setId(R.id.vm_boot_disk);
        biosTest = action("Probar BIOS", false, () -> start(NativeMachine.BOOT_SELF_TEST, false));
        biosTest.setId(R.id.vm_bios_test);
        pair(bootRow, bootDisk, biosTest);

        hardwareCheck = action("Comprobar equipo virtual", false, this::checkHardware);
        hardwareCheck.setId(R.id.vm_hardware_check);
        add(setup, hardwareCheck, 12);
        hardwareResult = label("", 13, MUTED);
        hardwareResult.setId(R.id.vm_hardware_result);
        hardwareResult.setVisibility(View.GONE);
        hardwareResult.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        add(setup, hardwareResult, 6);
        setContentView(scroll);
    }

    private void pickIso() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(picker, ISO_PICKER);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
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

    private void prepareDisk() {
        new AlertDialog.Builder(this)
                .setTitle("Crear disco virtual")
                .setMessage("El QSSD mostrará 64 GiB al invitado. El archivo del teléfono crece conforme Windows guarda datos.")
                .setNegativeButton("Cancelar", null)
                .setPositiveButton("Crear", (dialog, which) -> {
                    pending = true;
                    status.setText("Creando QSSD…");
                    status.setTextColor(TEXT);
                    updateControls();
                    worker.createDisk();
                })
                .show();
    }

    private void start(int bootSource, boolean debug) {
        if (session || pending) return;
        pending = true;
        fault = false;
        terminal = false;
        blankTextFrame = false;
        lastJson = "";
        monitor.reset();
        monitor.requestFocus();
        source.setText(switch (bootSource) {
            case NativeMachine.BOOT_ISO -> "BIOS · CD/DVD";
            case NativeMachine.BOOT_QSSD -> "BIOS · QSSD";
            default -> "BIOS · PRUEBA";
        });
        status.setText("Cargando medio de arranque…");
        status.setTextColor(TEXT);
        stats.setText("");
        updateControls();
        worker.start(bootSource, iso, debug);
        showBootStatus();
    }

    private void checkHardware() {
        if (session || pending) return;
        pending = true;
        checkingHardware = true;
        setHardwareResult("Comprobando equipo virtual y medios seleccionados…", MUTED);
        updateControls();
        worker.checkHardware(iso);
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
            JSONObject ram = preflight.optJSONObject("ram_profile");
            if (ram != null) {
                long mib = 1024L * 1024L;
                result.append("\nRAM utilizable por el invitado: ")
                        .append(ram.getLong("usable_bytes") / mib).append(" MiB")
                        .append("\nPresupuesto de respaldo en Android: ")
                        .append(ram.getLong("backing_budget_bytes") / mib).append(" MiB");
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
    public void onState(String json, boolean active, boolean hasSession, boolean stopped) {
        boolean wasFault = fault;
        pending = false;
        session = hasSession;
        running = active;
        terminal = stopped;
        if (!json.isEmpty()) lastJson = json;
        if (!session) {
            status.setTextColor(TEXT);
            status.setText("Máquina apagada");
            source.setText("BIOS · x86-64");
            fault = false;
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
                        case "halted" -> "La CPU invitada entró en espera";
                        case "powered_off" -> "El invitado se apagó";
                        case "fault" -> "Arranque detenido por un fallo";
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
        updateControls();
        if (fault && !wasFault) showBootStatus();
    }

    private void updateBootProgress(JSONObject diagnostic, JSONObject boot) {
        JSONObject machine = diagnostic.optJSONObject("machine");
        if (machine == null) machine = diagnostic;
        JSONObject ata = diagnostic.optJSONObject("ata");
        JSONObject volume = diagnostic.optJSONObject("disk");
        JSONObject ether = diagnostic.optJSONObject("ether_next");
        long steps = machine.optLong("instructions");
        long reads = machine.optLong("disk_reads");
        long readSectors = ata == null ? 0 : ata.optLong("read_sectors");
        long identifyTransfers = ata == null ? 0 : ata.optLong("identify_data_transfers");
        long batchedElements = ether == null ? 0 : ether.optLong("boot_rep_elements");
        long commits = volume == null ? 0 : volume.optLong("commits");
        boolean handedOff = boot == null ? steps > 0 : boot.optBoolean("handed_off");
        StringBuilder progress = new StringBuilder(handedOff ? "BIOS entregó el control al invitado"
                : "BIOS preparando el arranque");
        progress.append("\nModo ").append(modeName(machine.optString("cpu_mode")));
        progress.append(String.format(Locale.getDefault(), " · %,d instrucciones", steps));
        if (batchedElements > 0) {
            progress.append(String.format(Locale.getDefault(),
                    "\nEther · %,d elementos de copia o relleno en bloque", batchedElements));
        }
        if (boot != null && boot.optLong("mode_changes") > 0) {
            progress.append("\nÚltimo cambio: ").append(modeName(boot.optString("last_mode_from")))
                    .append(" → ").append(modeName(boot.optString("last_mode_to")));
        }
        long restarts = boot == null ? 0 : boot.optLong("restarts");
        if (restarts > 0) {
            progress.append(String.format(Locale.getDefault(), "\nReinicios automáticos: %,d", restarts));
        }
        if ("iso".equals(diagnostic.optString("source"))) {
            progress.append(String.format(Locale.getDefault(), "\nLecturas de la ISO por BIOS: %,d", reads));
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
        String detail = machine.optString("message");
        if ("fault".equals(diagnostic.optString("status")) && !detail.isEmpty()) {
            progress.append("\nÚltimo bloqueo: ").append(detail);
        } else if (session && handedOff && blankTextFrame) {
            progress.append("\nLa pantalla VGA todavía está en blanco.");
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
        scroll.post(() -> scroll.smoothScrollTo(0, 0));
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
        lastJson = "{\"schema\":\"reverse-windows-android-error-v1\",\"status\":\"fault\",\"message\":"
                + JSONObject.quote(message) + "}";
        updateControls();
        showBootStatus();
    }

    @Override
    public void onDiskCreated() {
        pending = false;
        status.setText("QSSD listo");
        status.setTextColor(TEXT);
        stats.setText("El disco virtual está vacío.");
        clearHardwareResult();
        updateControls();
    }

    private void updateControls() {
        boolean diskReady = disk.isFile();
        media.setText(iso == null ? "CD/DVD · sin ISO" : "CD/DVD · " + isoName);
        storage.setText(diskReady ? "QSSD · 64 GiB lógicos" : "QSSD · sin disco");
        boolean setup = !session && !pending;
        selectIso.setEnabled(setup);
        ejectIso.setEnabled(setup && iso != null);
        createDisk.setVisibility(diskReady ? View.GONE : View.VISIBLE);
        createDisk.setEnabled(setup);
        bootIso.setEnabled(setup && iso != null && diskReady);
        bootDisk.setEnabled(setup && diskReady);
        biosTest.setEnabled(setup);
        hardwareCheck.setEnabled(setup);
        controls.setVisibility(session ? View.VISIBLE : View.GONE);
        pause.setText(running ? "Pausar" : "Reanudar");
        pause.setEnabled(session && !pending && !fault && !terminal);
        stop.setEnabled(session && !pending);
        evidence.setVisibility(lastJson.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void copyEvidence() {
        ClipboardManager clipboard = getSystemService(ClipboardManager.class);
        clipboard.setPrimaryClip(ClipData.newPlainText("REVERSE Windows", lastJson));
        if (Build.VERSION.SDK_INT < 33) Toast.makeText(this, "Diagnóstico copiado", Toast.LENGTH_SHORT).show();
    }

    private void sendGuestKey(int ascii, int scan) {
        if (session && running && !pending && !fault) {
            worker.key(ascii, scan, true);
            worker.key(ascii, scan, false);
        }
    }

    @Override
    public boolean onKey(KeyEvent event) {
        if (!session || !running || pending || fault) return false;
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
        for (int index = 0; index < Math.min(text.length(), 256); index++) {
            char key = text.charAt(index);
            int scan = GuestKeys.scanAscii(key);
            if (scan == 0 || !session || !running || pending || fault) continue;
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

    private LinearLayout row(LinearLayout parent) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        parent.addView(layout, new LinearLayout.LayoutParams(-1, -2));
        return layout;
    }

    private TextView label(String value, int size, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private Button action(String value, boolean primary, Runnable onClick) {
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
        int enabled = primary ? 0xff092019 : TEXT;
        int disabled = primary ? 0xff798986 : 0xff7e8ea2;
        button.setTextColor(new ColorStateList(
                new int[][]{{-android.R.attr.state_enabled}, {}}, new int[]{disabled, enabled}));
        int background = primary ? ACCENT : 0xff202d3d;
        android.graphics.drawable.StateListDrawable fills = new android.graphics.drawable.StateListDrawable();
        fills.addState(new int[]{-android.R.attr.state_enabled}, panel(0xff1a252f, 9, BORDER));
        fills.addState(new int[]{}, panel(background, 9, primary ? ACCENT : BORDER));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x3377e5cb), fills, null));
        button.setOnClickListener(v -> onClick.run());
        return button;
    }

    private GradientDrawable panel(int color, int radius, int border) {
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

    private void weighted(LinearLayout row, Button view, int spacing) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, 1);
        params.leftMargin = dp(spacing);
        row.addView(view, params);
    }

    private void add(LinearLayout parent, View view, int spacing) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(spacing);
        parent.addView(view, params);
    }

    private int dp(int amount) {
        return Math.round(getResources().getDisplayMetrics().density * amount);
    }
}
