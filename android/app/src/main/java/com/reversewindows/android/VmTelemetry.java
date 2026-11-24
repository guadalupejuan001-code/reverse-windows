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
    private final LinearLayout residentPanel;
    private final TextView resident;
    private final TextView occupancy;
    private final ProgressBar meter;
    private final LinearLayout[] rows = new LinearLayout[19];
    private final TextView[] keys = new TextView[rows.length];
    private final TextView[] values = new TextView[rows.length];

    private Category category = Category.RAM;
    private JSONObject snapshot;
    private JSONObject machine;
    private String mode;
    private long steps;
    private long stepRate;
    private long motorRate;

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

        residentPanel = new LinearLayout(activity);
        residentPanel.setOrientation(VERTICAL);
        activity.add(this, residentPanel, 5);
        LinearLayout residentHeader = activity.row(residentPanel);
        TextView residentLabel = activity.label("Caché de páginas en RAM física", 12, MainActivity.TEXT);
        residentLabel.setTypeface(Typeface.DEFAULT_BOLD);
        residentHeader.addView(residentLabel, new LayoutParams(0, -2, 1));
        occupancy = activity.label("", 11, MainActivity.ERROR);
        occupancy.setTypeface(Typeface.MONOSPACE);
        residentHeader.addView(occupancy);
        resident = activity.label("", 15, MainActivity.TEXT);
        resident.setTypeface(Typeface.MONOSPACE);
        residentPanel.addView(resident);

        meter = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        meter.setIndeterminate(false);
        meter.setMax(1000);
        meter.setProgressTintList(ColorStateList.valueOf(MainActivity.ERROR));
        meter.setProgressBackgroundTintList(ColorStateList.valueOf(0x6688b5b7));
        meter.setContentDescription("Ocupación del presupuesto de caché de páginas");
        residentPanel.addView(meter, new LayoutParams(-1, activity.dp(5)));

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
                long instructions, long instructionsPerSecond, long motorInstructionsPerSecond) {
        snapshot = diagnostic;
        machine = guest;
        mode = modeName;
        steps = instructions;
        stepRate = instructionsPerSecond;
        motorRate = motorInstructionsPerSecond;
        if (isShown()) render();
    }

    void clear() {
        if (snapshot == null) return;
        snapshot = null;
        machine = null;
        render();
    }

    void render() {
        residentPanel.setVisibility(snapshot != null && category == Category.RAM ? View.VISIBLE : View.GONE);
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
        long usable = number(ram, "usable_bytes", number(machine, "ram_usable_bytes"));
        long used = residentWorkingSet(ram, machine);
        long limit = residentBudget(ram, machine);
        boolean etherRam = ram != null && "ether-state-ram".equals(ram.optString("memory_backend"));
        boolean knownOccupancy = used >= 0 && limit > 0;
        double ratio = knownOccupancy ? (double) used / limit : 0;
        heading(etherRam ? "RAM Éter lógica del invitado" : "RAM lógica del invitado",
                ramCapacity(usable), "");
        put(resident, residentUsage(used, limit));
        put(occupancy, knownOccupancy ? String.format(Locale.getDefault(), "%.1f %%", ratio * 100) : "");
        meter.setVisibility(knownOccupancy ? View.VISIBLE : View.GONE);
        if (knownOccupancy) meter.setProgress((int) Math.round(Math.min(ratio, 1) * 1000));
        long pssKiB = number(snapshot, "android_pss_kib");
        metric(0, "Tope de caché de páginas", storage(number(ram, "resident_ceiling_bytes")));
        metric(1, "Respaldo temporal QSSD", spillStatus(ram));
        metric(2, "Datos en el archivo temporal", storage(number(ram, "spill_file_payload_bytes")));
        metric(3, "Datos lógicos desplazados a QSSD", storage(number(ram, "spilled_bytes")));
        metric(4, "Datos de páginas residentes", storage(number(ram, "resident_payload_bytes",
                number(ram, "resident_bytes"))));
        metric(5, "Caché de lectura QSSD · usada / límite",
                storage(number(ram, "resident_cache_bytes", number(ram, "spill_cache_bytes"))) + " / "
                + storage(number(ram, "spill_cache_capacity_bytes")));
        metric(6, "Lecturas / escrituras de respaldo", count(number(ram, "spill_reads"))
                + " / " + count(number(ram, "spill_writes")));
        metric(7, "Tiempo en archivo · lectura / escritura", units(number(ram, "spill_read_us"), 1000, "ms", 3)
                + " / " + units(number(ram, "spill_write_us"), 1000, "ms", 3));
        metric(8, "Aciertos de caché de lectura QSSD", count(number(ram, "spill_cache_hits")));
        metric(9, "Metadatos de RAM", storage(number(ram, "resident_metadata_bytes")));
        metric(10, etherRam ? "Páginas con estado Éter" : "Tablas de páginas",
                count(number(ram, "page_state_vectors")));
        metric(11, "Proceso Android (PSS)", pssKiB > 0 ? units(pssKiB, KIB, "MiB", 1) : UNKNOWN);
        long transfers = number(ram, "ram_data_transfers_per_second");
        String ramClock = frequency(number(ram, "ram_clock_hz"));
        metric(12, transfers >= 0 ? "RAM DDR5 virtual · CK y transferencias" : "Reloj de la RAM virtual",
                transfers >= 0 ? ramClock + " · " + units(transfers, 1_000_000, "MT/s", 0) : ramClock);
        JSONObject pressure = ram == null ? null : ram.optJSONObject("access_pressure");
        if (pressure == null) { showRows(13); return; }
        metric(13, "Escrituras que necesitaron espacio", count(number(pressure, "forced_reclaims")));
        metric(14, "Tiempo consiguiendo espacio · total",
                units(number(pressure, "forced_reclaim_ns"), 1_000_000, "ms", 3));
        metric(15, "Mayor demora al conseguir espacio",
                units(number(pressure, "forced_reclaim_max_ns"), 1_000_000, "ms", 3));
        long timedReads = number(pressure, "backing_timed_attempts");
        if (timedReads <= 0) { showRows(16); return; }
        metric(16, "Lecturas cronometradas", count(timedReads));
        metric(17, "Lecturas cronometradas · total / máximo",
                units(number(pressure, "backing_read_ns"), 1_000_000, "ms", 3) + " / "
                + units(number(pressure, "backing_read_max_ns"), 1_000_000, "ms", 3));
        metric(18, "Espera de bloqueo del respaldo",
                units(number(pressure, "backing_lock_wait_ns"), 1_000_000, "ms", 3));
        showRows(19);
    }

    static void appendRamSummary(StringBuilder text, JSONObject ram) {
        if (ram == null) return;
        text.append("\nRAM lógica del invitado · ").append(ramCapacity(number(ram, "usable_bytes")))
                .append("\nCaché de páginas en RAM física · ")
                .append(residentUsage(residentWorkingSet(ram, null), residentBudget(ram, null)))
                .append("\nRespaldo temporal QSSD · ").append(spillStatus(ram));
        if ("file".equals(ram.optString("spill_mode"))) {
            text.append(" · ").append(storage(number(ram, "spill_file_payload_bytes"))).append(" en archivo");
        } else if ("unavailable".equals(ram.optString("spill_mode"))) {
            String detail = ram.optString("spill_error");
            if (!detail.isEmpty()) text.append('\n').append(detail);
        }
    }

    private static long residentWorkingSet(JSONObject ram, JSONObject machine) {
        long workingSet = number(ram, "resident_working_set_bytes");
        if (workingSet >= 0) return workingSet;
        long payload = number(ram, "resident_bytes", number(machine, "ram_allocated_bytes"));
        return payload < 0 ? payload : payload + number(ram, "spill_cache_bytes", 0);
    }

    private static long residentBudget(JSONObject ram, JSONObject machine) {
        return number(ram, "resident_budget_bytes",
                number(ram, "backing_budget_bytes", number(machine, "ram_budget_bytes")));
    }

    private static String residentUsage(long used, long limit) {
        return units(used, MIB, "MiB", 1) + " / " + units(limit, MIB, "MiB", 1);
    }

    private static String ramCapacity(long bytes) {
        if (bytes >= GIB && bytes % GIB == 0) return units(bytes, GIB, "GiB", 0);
        return storage(bytes);
    }

    private static String spillStatus(JSONObject ram) {
        if (ram == null) return UNKNOWN;
        return switch (ram.optString("spill_mode")) {
            case "file" -> "Límite " + storage(number(ram, "spill_limit_bytes"));
            case "unavailable" -> "No disponible";
            case "disabled" -> "Desactivado";
            default -> UNKNOWN;
        };
    }

    private void renderCpu() {
        JSONObject ether = snapshot.optJSONObject("ether_arm64");
        JSONObject topology = snapshot.optJSONObject("cpu_topology");
        long elapsed = number(snapshot, "elapsed_ms");
        long sessionRate = elapsed > 0 && steps >= 0 ? Math.round(steps * 1000.0 / elapsed) : -1;
        if (stepRate >= 0) {
            String active = motorRate >= 0 ? "Dentro del motor " + formatRate(motorRate) + " pasos/s · " : "";
            heading("Pasos/s en tiempo real", formatRate(stepRate),
                    active + "Desde el inicio " + formatRate(sessionRate) + " pasos/s · " + mode);
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
        metric(5, "Reloj TSC del invitado", frequency(number(topology, "guest_tsc_hz")));
        JSONObject host = snapshot.optJSONObject("host_cpu");
        long low = number(host, "min_reported_khz");
        long high = number(host, "max_reported_khz");
        String range = high <= 0 ? "Sin lectura" : units(high, 1000, "MHz", 0);
        if (low > 0 && low < high) range = formatted(low, 1000, 0) + "–" + range;
        metric(6, "CPU física reportada", range);
        long threads = number(host, "hint_threads", 0);
        metric(7, "Solicitud a Android", threads == 0 ? "Inactiva" : count(threads) + " hilos");
        JSONObject loops = snapshot.optJSONObject("ether_next");
        int row = 8;
        if (number(loops, "loop_time_samples", 0) > 0) {
            metric(row++, "Último bloque Éter muestreado", units(number(loops, "loop_last_ns"), 1_000_000, "ms", 6)
                    + " · " + count(number(loops, "loop_last_steps")) + " pasos");
        }
        if (number(topology, "off_bus_ram_steps") >= 0) {
            metric(row++, "Pasos de RAM ejecutados fuera del bus", count(number(topology, "off_bus_ram_steps")));
            metric(row++, "Bloques de RAM descartados por conflicto", count(number(topology, "ram_view_conflicts")));
        }
        long samples = number(topology, "sampled_bus_grants", 0);
        if (samples > 0) {
            long busy = number(topology, "sampled_bus_contentions", 0);
            metric(row++, "Intentos con bus ocupado (muestra)", count(busy) + " / " + count(samples));
            if (busy > 0) {
                metric(row++, "Espera media al encontrar el bus ocupado",
                        units(number(topology, "sampled_bus_wait_ns") / busy, 1_000_000, "ms", 6));
            }
        }
        showRows(row);
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
            long read = number(disk, "sequential_read_reference_bytes_per_second");
            long write = number(disk, "sequential_write_reference_bytes_per_second");
            if (read >= 0 && write >= 0) {
                metric(rows++, "Referencia QSSD secuencial · L/E",
                        units(read, 1_000_000_000, "GB/s", 1) + " / " + units(write, 1_000_000_000, "GB/s", 1));
            }
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
        if (gpu == null || !gpu.has("qssd_ready")) { showRows(8); return; }
        metric(8, "Captura VBE en memoria", storage(number(vbe, "scanout_cache_bytes")));
        metric(9, "Respaldo qSSD de VRAM", gpu.optBoolean("qssd_ready")
                ? storage(number(gpu, "qssd_backing_bytes")) : "No disponible");
        metric(10, "Reloj virtual de VRAM", frequency(number(gpu, "vram_clock_hz")));
        JSONObject quantum = gpu.optJSONObject("quantum_vram");
        metric(11, "Bytes de base Éter guardados / restaurados", storage(number(quantum, "saved_bytes"))
                + " / " + storage(number(quantum, "restored_bytes")));
        if (!gpu.has("renderer_ready_at_power_on")) { showRows(12); return; }
        metric(12, "GLES al encender la placa", gpu.optBoolean("renderer_ready_at_power_on")
                ? "Listo antes de la primera instrucción" : "Sin preparar");
        metric(13, "Estados compartidos guardados / restaurados", count(number(quantum, "state_saves"))
                + " / " + count(number(quantum, "state_restores")));
        metric(14, "Último estado Éter transferido", storage(number(quantum, "last_state_bytes")));
        if (!gpu.has("backend_frames")) { showRows(15); return; }
        metric(15, "Cuadros enviados a GLES / reutilizados", count(number(gpu, "backend_frames"))
                + " / " + count(number(gpu, "replayed_frames")));
        metric(16, "Lecturas de píxeles evitadas", storage(number(gpu, "avoided_readback_bytes")));
        metric(17, "Triángulos omitidos por la máscara Éter", count(number(gpu, "ether_mask_rejected_triangles")));
        if (host == null || !host.has("background_pixels_skipped")) { showRows(18); return; }
        metric(18, "Píxeles de fondo omitidos bajo VBE", count(number(host, "background_pixels_skipped")));
        showRows(19);
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
