package com.reversewindows.android;

import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.Locale;

final class VmTelemetry extends LinearLayout {
    private static final long KIB = 1024;
    private static final long MIB = 1024 * KIB;
    private static final long GIB = 1024 * MIB;
    private static final long PIB = (1L << 20) * GIB;
    private static final String UNKNOWN = "—";

    private enum Category {
        RAM("RAM"), CPU("CPU"), DISK("Disco"), VIDEO("Video");

        final String label;

        Category(String label) {
            this.label = label;
        }
    }

    private final Button[] tabs = new Button[Category.values().length];
    private final TextView caption;
    private final TextView headline;
    private final TextView note;
    private final LinearLayout backing;
    private final TextView resident;
    private final TextView occupancy;
    private final ProgressBar meter;
    private final LinearLayout[] rows = new LinearLayout[8];
    private final TextView[] keys = new TextView[rows.length];
    private final TextView[] values = new TextView[rows.length];

    private Category category = Category.RAM;
    private JSONObject snapshot;
    private JSONObject machine;
    private String mode;
    private long steps;
    private long stepRate;

    VmTelemetry(MainActivity activity) {
        super(activity);
        setOrientation(VERTICAL);
        int padding = activity.dp(9);
        setPadding(padding, padding, padding, padding);
        setBackground(activity.panel(0x48376560, 10, MainActivity.BORDER));

        TextView title = activity.label("Telemetría", 14, MainActivity.TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        addView(title);
        LinearLayout tabBar = activity.row(this);
        ((LayoutParams) tabBar.getLayoutParams()).topMargin = activity.dp(4);
        for (Category item : Category.values()) {
            Button tab = activity.action(item.label, false, () -> select(item));
            tab.setTextSize(12);
            tab.setPadding(activity.dp(2), activity.dp(4), activity.dp(2), activity.dp(4));
            activity.weighted(tabBar, tab, item.ordinal() == 0 ? 0 : 4);
            tabs[item.ordinal()] = tab;
        }

        caption = activity.label("", 12, MainActivity.TEXT);
        caption.setTypeface(Typeface.DEFAULT_BOLD);
        activity.add(this, caption, 5);
        headline = activity.label("", 25, MainActivity.ACCENT);
        headline.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        headline.setIncludeFontPadding(false);
        addView(headline);
        note = activity.label("", 11, MainActivity.MUTED);
        addView(note);

        backing = new LinearLayout(activity);
        backing.setOrientation(VERTICAL);
        activity.add(this, backing, 5);
        LinearLayout backingHeader = activity.row(backing);
        TextView residentLabel = activity.label("Datos de RAM residentes", 12, MainActivity.TEXT);
        residentLabel.setTypeface(Typeface.DEFAULT_BOLD);
        backingHeader.addView(residentLabel, new LayoutParams(0, -2, 1));
        occupancy = activity.label("", 11, MainActivity.ERROR);
        occupancy.setTypeface(Typeface.MONOSPACE);
        backingHeader.addView(occupancy);
        resident = activity.label("", 15, MainActivity.TEXT);
        resident.setTypeface(Typeface.MONOSPACE);
        backing.addView(resident);

        meter = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        meter.setIndeterminate(false);
        meter.setMax(1000);
        meter.setProgressTintList(ColorStateList.valueOf(MainActivity.ERROR));
        meter.setProgressBackgroundTintList(ColorStateList.valueOf(0x6688b5b7));
        meter.setContentDescription("Ocupación del presupuesto de RAM residente");
        backing.addView(meter, new LayoutParams(-1, activity.dp(5)));

        for (int index = 0; index < rows.length; index++) {
            LinearLayout row = activity.row(this);
            ((LayoutParams) row.getLayoutParams()).topMargin = activity.dp(3);
            row.setGravity(Gravity.CENTER_VERTICAL);
            TextView key = activity.label("", 11, MainActivity.MUTED);
            row.addView(key, new LayoutParams(0, -2, 1));
            TextView value = activity.label("", 11, MainActivity.TEXT);
            value.setTypeface(Typeface.MONOSPACE);
            value.setGravity(Gravity.END);
            value.setPadding(activity.dp(4), 0, 0, 0);
            row.addView(value, new LayoutParams(-2, -2));
            rows[index] = row;
            keys[index] = key;
            values[index] = value;
        }
        select(Category.RAM);
    }

    private void select(Category next) {
        category = next;
        for (Category item : Category.values()) {
            tabs[item.ordinal()].setSelected(item == category);
        }
        render();
    }

    void update(JSONObject diagnostic, JSONObject guest, String modeName,
                long instructions, long instructionsPerSecond) {
        snapshot = diagnostic;
        machine = guest;
        mode = modeName;
        steps = instructions;
        stepRate = instructionsPerSecond;
        if (isShown()) render();
    }

    void clear() {
        if (snapshot == null) return;
        snapshot = null;
        machine = null;
        render();
    }

    void render() {
        backing.setVisibility(snapshot != null && category == Category.RAM ? View.VISIBLE : View.GONE);
        if (snapshot == null) {
            heading(category.label, UNKNOWN, "Sin sesión");
            showRows(0);
            return;
        }
        switch (category) {
            case RAM -> renderRam();
            case CPU -> renderCpu();
            case DISK -> renderDisk();
            case VIDEO -> renderVideo();
        }
    }

    private void renderRam() {
        JSONObject ram = snapshot.optJSONObject("ram_profile");
        long logical = number(ram, "address_space_bytes", number(machine, "ram_bytes"));
        long usable = number(ram, "usable_bytes", number(machine, "ram_usable_bytes"));
        long used = number(ram, "resident_bytes", number(machine, "ram_allocated_bytes"));
        long limit = number(ram, "backing_budget_bytes", number(machine, "ram_budget_bytes"));
        long rawCapacity = number(ram, "raw_backing_capacity_bytes");
        boolean knownOccupancy = used >= 0 && limit > 0;
        double ratio = knownOccupancy ? (double) used / limit : 0;
        heading("RAM utilizable por el invitado", units(usable >= 0 ? usable : logical, GIB, "GiB", 2),
                rawCapacity >= 0 ? "Capacidad estimada sin comprimir · " + units(rawCapacity, GIB, "GiB", 2) : "");
        put(resident, formatted(used, MIB, 1) + " / " + formatted(limit, MIB, 1) + " MiB");
        put(occupancy, knownOccupancy ? String.format(Locale.getDefault(), "%.1f %%", ratio * 100) : "");
        meter.setVisibility(knownOccupancy ? View.VISIBLE : View.GONE);
        if (knownOccupancy) meter.setProgress((int) Math.round(Math.min(ratio, 1) * 1000));
        long pssKiB = number(snapshot, "android_pss_kib");
        metric(0, "Datos lógicos en respaldo", units(number(ram, "spilled_bytes"), MIB, "MiB", 1));
        metric(1, "Datos en el archivo temporal", storage(number(ram, "spill_file_payload_bytes")));
        metric(2, "Metadatos de RAM", storage(number(ram, "resident_metadata_bytes")));
        metric(3, "Vectores de páginas", count(number(ram, "page_state_vectors")));
        metric(4, "Proceso Android (PSS)", pssKiB > 0 ? units(pssKiB, KIB, "MiB", 1) : UNKNOWN);
        metric(5, "Reloj de la RAM virtual", frequency(number(ram, "ram_clock_hz")));
        showRows(6);
    }

    private void renderCpu() {
        JSONObject ether = snapshot.optJSONObject("ether_arm64");
        JSONObject topology = snapshot.optJSONObject("cpu_topology");
        long elapsed = number(snapshot, "elapsed_ms");
        long sessionRate = elapsed > 0 && steps >= 0 ? Math.round(steps * 1000.0 / elapsed) : -1;
        if (stepRate >= 0) {
            heading("Pasos/s durante las tandas", formatRate(stepRate),
                    "Desde el inicio " + formatRate(sessionRate) + " pasos/s · " + mode);
        } else {
            heading("Pasos/s desde el inicio", formatRate(sessionRate), mode);
        }
        metric(0, "Pasos x86 de la sesión", count(steps));
        long nativeSteps = number(topology, "phase_ether_arm64_steps");
        metric(1, nativeSteps >= 0 ? "Pasos Éter ARM64 de la fase" : "Pasos Éter ARM64 de la vCPU de arranque",
                count(nativeSteps >= 0 ? nativeSteps : number(ether, "guest_steps")));
        metric(2, "Pasos proyectados por vectores", count(number(topology, "phase_projected_steps")));
        metric(3, "Bloques encadenados en paralelo", count(number(topology, "parallel_register_continuations")));
        metric(4, "vCPU iniciadas / configuradas", count(number(topology, "started")) + " / "
                + count(number(topology, "logical_processors")));
        metric(5, "Reloj virtual (TSC)", frequency(number(topology, "guest_tsc_hz")));
        JSONObject host = snapshot.optJSONObject("host_cpu");
        long low = number(host, "min_reported_khz");
        long high = number(host, "max_reported_khz");
        String range = high <= 0 ? "Sin lectura" : units(high, 1000, "MHz", 0);
        if (low > 0 && low < high) range = formatted(low, 1000, 0) + "–" + range;
        metric(6, "CPU física reportada", range);
        long threads = number(host, "hint_threads", 0);
        metric(7, "Solicitud a Android", threads == 0 ? "Inactiva" : count(threads) + " hilos");
        showRows(8);
    }

    private static String formatRate(long stepsPerSecond) {
        return stepsPerSecond >= 1_000_000
                ? units(stepsPerSecond, 1_000_000, "M", 2) : count(stepsPerSecond);
    }

    private void renderDisk() {
        JSONObject disk = snapshot.optJSONObject("disk");
        JSONObject io = snapshot.optJSONObject("storage_io");
        JSONObject optical = io == null ? null : io.optJSONObject("bios_boot_media");
        JSONObject atapi = snapshot.optJSONObject("atapi");
        boolean iso = "iso".equals(snapshot.optString("source"));
        long capacity = number(disk, "logical_bytes");
        long biosBytes = number(optical, "read_bytes");
        long atapiBytes = number(atapi, "read_bytes", number(atapi, "read_sectors", 0) * 2048);
        long isoBytesRead = biosBytes >= 0 ? biosBytes + atapiBytes : -1;
        heading(iso ? "ISO leída en esta fase" : "Disco lógico QSSD",
                iso ? units(isoBytesRead, MIB, "MiB", 1) : units(capacity, GIB, "GiB", 1),
                iso ? "BIOS " + units(biosBytes, MIB, "MiB", 1) + " · ATAPI "
                        + units(atapiBytes, MIB, "MiB", 1) : "");
        metric(0, iso ? "Disco lógico QSSD" : "Lecturas por BIOS",
                iso ? units(capacity, GIB, "GiB", 1) : count(number(machine, "disk_reads")));
        metric(1, "Archivo QSSD en el teléfono", storage(number(disk, "physical_bytes")));
        metric(2, "Operaciones guardadas", count(number(disk, "commits")));
        int rows = 3;
        long ataHz = number(snapshot.optJSONObject("ata"), "clock_hz");
        if (ataHz > 0) {
            metric(rows++, "Reloj del controlador ATA virtual", frequency(ataHz));
        }
        if (disk != null && disk.has("clock_hz")) {
            metric(rows++, "Reloj del QSSD virtual", frequency(number(disk, "clock_hz")));
        }
        JSONObject dataDisk = snapshot.optJSONObject("data_disk");
        if (dataDisk != null) {
            metric(rows++, "QSSD de datos", storage(number(dataDisk, "logical_bytes")) + " · "
                    + storage(number(dataDisk, "physical_bytes")) + " en Android");
        } else if (iso) {
            metric(rows++, "Tamaño de la ISO", units(number(snapshot, "iso_bytes"), GIB, "GiB", 2));
        }
        showRows(rows);
    }

    private void renderVideo() {
        JSONObject gpu = snapshot.optJSONObject("guest_gpu");
        JSONObject vbe = snapshot.optJSONObject("vbe");
        JSONObject host = snapshot.optJSONObject("host_vbe");
        String backend = host == null ? "" : host.optString("last_backend");
        String renderer = gpu == null ? "" : gpu.optString("renderer");
        heading("Resolución del invitado", resolution(machine), machine.optString("video_kind"));
        metric(0, "Cuadros VBE", count(number(vbe, "frames")));
        metric(1, "Framebuffer VBE del invitado", "vbe32".equals(machine.optString("video_kind"))
                ? storage(number(machine, "video_bytes")) : UNKNOWN);
        metric(2, "Último dibujo VBE en Android", switch (backend) {
            case "render_nodes" -> "HWUI por franjas";
            case "hardware_canvas" -> "Canvas acelerado";
            case "software_canvas" -> "Canvas por software";
            default -> UNKNOWN;
        });
        metric(3, "Reloj virtual de la GPU C08", frequency(number(gpu, "virtual_clock_hz")));
        metric(4, "VRAM lógica C08", units(number(gpu, "logical_vram_bytes"), 1_000_000_000, "GB", 1));
        metric(5, "Caché VRAM residente C08", storage(number(gpu, "resident_vram_bytes")));
        metric(6, "Dibujos 3D completados por C08", count(number(gpu, "draws")));
        if (renderer.isEmpty() && number(gpu, "draw_requests") == 0 &&
                number(gpu, "scheduler_submissions") == 0) renderer = "Sin solicitudes";
        metric(7, "Renderizador 3D C08", renderer.isEmpty() ? UNKNOWN : renderer);
        showRows(8);
    }

    private void heading(String title, String value, String hint) {
        put(caption, title);
        put(headline, value);
        put(note, hint);
        note.setVisibility(hint.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void metric(int index, String name, String value) {
        put(keys[index], name);
        put(values[index], value);
    }

    private void showRows(int count) {
        for (int index = 0; index < rows.length; index++) {
            rows[index].setVisibility(index < count ? View.VISIBLE : View.GONE);
        }
    }

    private static void put(TextView view, String value) {
        if (!TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    private static long number(JSONObject object, String key) {
        return number(object, key, -1);
    }

    private static long number(JSONObject object, String key, long fallback) {
        return object == null ? fallback : object.optLong(key, fallback);
    }

    private static String count(long value) {
        return value < 0 ? UNKNOWN : String.format(Locale.getDefault(), "%,d", value);
    }

    private static String formatted(long value, long divisor, int decimals) {
        return value < 0 ? UNKNOWN : String.format(Locale.getDefault(), "%,." + decimals + "f",
                (double) value / divisor);
    }

    private static String units(long value, long divisor, String name, int decimals) {
        return value < 0 ? UNKNOWN : formatted(value, divisor, decimals) + " " + name;
    }

    private static String frequency(long hz) {
        if (hz >= 1_000_000_000_000L) return units(hz, 1_000_000_000_000L, "THz", 1);
        if (hz >= 1_000_000_000L) return units(hz, 1_000_000_000L, "GHz", 3);
        return units(hz, 1_000_000, "MHz", 2);
    }

    private static String storage(long bytes) {
        if (bytes >= PIB) return units(bytes, PIB, "PiB", 1);
        if (bytes >= GIB) return units(bytes, GIB, "GiB", 1);
        if (bytes >= MIB) return units(bytes, MIB, "MiB", 1);
        if (bytes >= KIB) return units(bytes, KIB, "KiB", 1);
        return bytes < 0 ? UNKNOWN : count(bytes) + " B";
    }

    private static String resolution(JSONObject machine) {
        long width = number(machine, "video_width");
        long height = number(machine, "video_height");
        return width > 0 && height > 0 ? width + " × " + height : UNKNOWN;
    }

    static String summary(JSONObject diagnostic, JSONObject machine, long steps) {
        String summary = count(steps) + " pasos";
        long ms = number(diagnostic, "elapsed_ms");
        if (ms >= 0) {
            long seconds = ms / 1000;
            summary = String.format(Locale.getDefault(), "%02d:%02d", seconds / 60, seconds % 60)
                    + " · " + summary;
        }
        String dimensions = resolution(machine);
        return UNKNOWN.equals(dimensions) ? summary : summary + " · " + dimensions;
    }
}
