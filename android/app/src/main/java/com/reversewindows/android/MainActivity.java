package com.reversewindows.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.database.Cursor;
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

    private File disk;
    private SharedPreferences prefs;
    private Uri iso;
    private String isoName;
    private VmWorker worker;
    private GuestMonitor monitor;
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
    private LinearLayout controls;
    private boolean pending;
    private boolean session;
    private boolean running;
    private boolean fault;
    private boolean terminal;
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
        worker = new VmWorker(getContentResolver(), disk, this);
        buildScreen();
        updateControls();
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra(
                "com.reversewindows.android.SELF_TEST", false)) {
            start(NativeMachine.BOOT_SELF_TEST, true);
        }
    }

    private void buildScreen() {
        ScrollView scroll = new ScrollView(this);
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
        monitor = new GuestMonitor(this, this);
        monitor.setBackground(panel(0xff080b10, 9, BORDER));
        monitor.setClipToOutline(true);
        int availableWidth = getResources().getConfiguration().screenWidthDp;
        int displayHeight = Math.max(200, Math.min(370, (availableWidth - 62) * 5 / 8));
        display.addView(monitor, new LinearLayout.LayoutParams(-1, dp(displayHeight)));

        status = label("Máquina apagada", 16, TEXT);
        status.setId(R.id.vm_status);
        status.setTypeface(Typeface.DEFAULT_BOLD);
        add(display, status, 12);
        stats = label("", 12, MUTED);
        stats.setTypeface(Typeface.MONOSPACE);
        stats.setTextIsSelectable(true);
        add(display, stats, 5);

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

        evidence = action("Copiar diagnóstico", false, this::copyEvidence);
        add(body, evidence, 10);
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
        lastJson = "";
        monitor.reset();
        monitor.requestFocus();
        source.setText(switch (bootSource) {
            case NativeMachine.BOOT_ISO -> "BIOS · CD/DVD";
            case NativeMachine.BOOT_QSSD -> "BIOS · QSSD";
            default -> "BIOS · PRUEBA";
        });
        status.setText("Iniciando máquina…");
        status.setTextColor(TEXT);
        stats.setText("");
        updateControls();
        worker.start(bootSource, iso, debug);
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
                JSONObject evidence = new JSONObject(json);
                JSONObject machine = evidence.optJSONObject("machine");
                if (machine == null) machine = evidence;
                String code = evidence.optString("status");
                if (session) {
                    fault = "fault".equals(code);
                    status.setTextColor(fault ? ERROR : TEXT);
                    String message = switch (code) {
                        case "waiting_key" -> "BIOS espera una tecla";
                        case "halted" -> "CPU invitada en HLT";
                        case "powered_off" -> "El invitado se apagó";
                        case "fault" -> "El invitado se detuvo";
                        default -> "En ejecución";
                    };
                    status.setText(!active && !fault && !terminal
                            ? "Máquina en pausa" : message);
                }
                String mode = machine.optString("cpu_mode", "CPU");
                String rip = machine.optString("rip_hex", "");
                String detail = machine.optString("message", "");
                String position = String.format(Locale.US, "%s · CS %04X · IP %s\n%,d pasos",
                        mode, machine.optInt("cs", 0), rip, machine.optLong("instructions"));
                if (fault && !detail.isEmpty()) position += "\n" + detail;
                stats.setText(position);
            } catch (JSONException error) {
                stats.setText("Diagnóstico disponible para copiar.");
            }
        }
        updateControls();
    }

    @Override
    public void onFrame(VmWorker.Frame frame) {
        monitor.show(frame);
    }

    @Override
    public void onError(String message, boolean hasSession) {
        pending = false;
        session = hasSession;
        running = false;
        fault = true;
        status.setTextColor(ERROR);
        status.setText("Máquina detenida");
        stats.setText(message);
        updateControls();
    }

    @Override
    public void onDiskCreated() {
        pending = false;
        status.setText("QSSD listo");
        status.setTextColor(TEXT);
        stats.setText("El disco virtual está vacío.");
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
        controls.setVisibility(session ? View.VISIBLE : View.GONE);
        pause.setText(running ? "Pausar" : "Reanudar");
        pause.setEnabled(session && !pending && !fault && !terminal);
        stop.setEnabled(session && !pending);
        evidence.setEnabled(!lastJson.isEmpty());
    }

    private void copyEvidence() {
        ClipboardManager clipboard = getSystemService(ClipboardManager.class);
        clipboard.setPrimaryClip(ClipData.newPlainText("REVERSE Windows", lastJson));
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
