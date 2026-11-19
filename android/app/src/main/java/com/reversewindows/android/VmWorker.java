package com.reversewindows.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Debug;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.util.AtomicFile;
import android.util.Log;
import android.view.Choreographer;

import org.json.JSONObject;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

final class VmWorker {
    interface Listener {
        void onState(String json, boolean running, boolean hasSession, boolean stopped, boolean detailed);
        void onFrame(Frame frame);
        void onError(String message, boolean hasSession);
        void onDiskCreated(boolean data);
        void onHardwareChecked(String json);
        void onSavedDiagnostic(String json);
        void onDiagnosticExported(boolean saved);
    }

    static final class Frame {
        final int width;
        final int height;
        final byte[] text;
        final int[] video;

        Frame(int[] video, byte[] text) {
            this.width = video[0];
            this.height = video[1];
            this.text = video.length == 2 ? text.clone() : null;
            this.video = this.text == null ? video : null;
        }
    }

    private static final String TAG = "ReverseWindows";
    private static final String BUILD_METADATA =
            "\"app_version\":" + JSONObject.quote(BuildConfig.VERSION_NAME) +
            ",\"app_version_code\":" + BuildConfig.VERSION_CODE +
            ",\"application_id\":" + JSONObject.quote(BuildConfig.APPLICATION_ID) +
            ",\"mundo_x_sha\":" + JSONObject.quote(BuildConfig.MUNDO_X_SHA);
    private static final int DISK_GIB = 64;
    private static final int DATA_DISK_GIB = 8 * 1024 * 1024;
    private static final int INITIAL_SLICE_STEPS = 4096;
    private static final int MIN_SLICE_STEPS = 64;
    private static final int MAX_SLICE_STEPS = Integer.MAX_VALUE;
    private static final long TARGET_SLICE_NS = 8_000_000L;
    private static final long HALTED_POLL_DELAY_MS = 35;
    private static final long STATUS_INTERVAL_MS = 350;
    private static final long DIAGNOSTIC_INTERVAL_MS = 5_000;
    private static final long ANDROID_SAMPLE_INTERVAL_MS = 10_000;
    // Newer Android versions no longer send the running-low trim levels.
    private static final long RAM_SAMPLE_INTERVAL_MS = 10_000;
    private static final long RAM_RECLAIM_SAMPLE_INTERVAL_MS = 1_000;
    private static final long RAM_RECLAIM_INTERVAL_MS = 16;
    private static final long RAM_PREPARE_INTERVAL_MS = 50;
    private static final String DIAGNOSTIC_FILE = "ultimo-diagnostico.json";

    private final Context context;
    private final ContentResolver resolver;
    private final PowerManager powerManager;
    private final File disk;
    private final File dataDisk;
    private final AtomicFile diagnosticFile;
    private final Listener listener;
    private final HandlerThread thread = new HandlerThread("reverse-windows-vm");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final byte[] textCells = new byte[4000];

    private volatile boolean released;
    private volatile boolean foreground = true;
    private volatile long handle;
    private int outcome;
    private int sliceSteps = INITIAL_SLICE_STEPS;
    private boolean running;
    private boolean debugSelfTest;
    private long lastStatus;
    private long lastDiagnosticSave;
    private long sessionStarted;
    private long sessionWorkerCpuStartNs = -1;
    private long sessionProcessCpuStartMs;
    private long nextAndroidSample;
    private long androidPssKiB;
    private int androidThermalStatus = -1;
    private long stepNanos;
    private long lastSliceNanos;
    private int lastSliceSteps;
    private long lastSliceCompleted;
    private long frameNanos;
    private long statusNanos;
    private long statusSnapshots;
    private long diagnosticWrites;
    private long diagnosticWriteNanos;
    private boolean frameRequested; // Worker owns the capture until the UI applies or discards it.
    private boolean videoDirty;
    private String lastLoggedStatus = "";
    private String lastDiagnostic = "";
    private String persistedDiagnostic = "";
    private final Runnable slice = this::runSlice;
    private final Runnable ramMaintenance = this::maintainRam;
    private boolean reclaimRam;
    private boolean reclaimPending;
    private long nextRamSample;

    VmWorker(Context context, File disk, File dataDisk, Listener listener) {
        this.context = context.getApplicationContext();
        this.resolver = this.context.getContentResolver();
        this.powerManager = this.context.getSystemService(PowerManager.class);
        this.disk = disk;
        this.dataDisk = dataDisk;
        this.diagnosticFile = new AtomicFile(new File(this.context.getFilesDir(), DIAGNOSTIC_FILE));
        this.listener = listener;
        thread.start();
        worker = new Handler(thread.getLooper());
    }

    void loadDiagnostic() {
        worker.post(() -> {
            try {
                String json = new String(diagnosticFile.readFully(), StandardCharsets.UTF_8);
                persistedDiagnostic = json;
                send(() -> listener.onSavedDiagnostic(json));
            } catch (FileNotFoundException ignored) {
                // The app has not recorded a VM session yet.
            } catch (IOException error) {
                Log.w(TAG, "No se pudo leer el último diagnóstico", error);
            }
        });
    }

    void exportDiagnostic(Uri destination, String json) {
        worker.post(() -> {
            try (OutputStream stream = resolver.openOutputStream(destination, "wt")) {
                if (stream == null) throw new IOException("El destino no se pudo abrir.");
                stream.write(json.getBytes(StandardCharsets.UTF_8));
            } catch (IOException | RuntimeException error) {
                Log.w(TAG, "No se pudo guardar el diagnóstico", error);
                send(() -> listener.onDiagnosticExported(false));
                return;
            }
            send(() -> listener.onDiagnosticExported(true));
        });
    }

    void requestDiagnostic(Consumer<String> ready) {
        worker.post(() -> {
            String json = lastDiagnostic;
            try {
                if (handle != 0 && outcome != NativeMachine.FAULT) {
                    json = currentDiagnostic(true);
                    lastDiagnostic = json;
                    saveDiagnostic(json);
                }
            } catch (RuntimeException | LinkageError error) {
                Log.w(TAG, "No se pudo actualizar el diagnóstico", error);
            }
            final String result = json.isEmpty() ? persistedDiagnostic : json;
            send(() -> ready.accept(result));
        });
    }

    void createDisk(boolean data) {
        worker.post(() -> {
            if (released || handle != 0) return;
            try {
                File image = data ? dataDisk : disk;
                NativeMachine.createDisk(image.getAbsolutePath(), data ? DATA_DISK_GIB : DISK_GIB);
                send(() -> listener.onDiskCreated(data));
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void checkHardware(Uri iso, boolean primeRam) {
        worker.post(() -> {
            if (released || handle != 0) return;
            try {
                String diskPath = disk.exists() ? disk.getAbsolutePath() : "";
                String result;
                if (iso == null) {
                    result = NativeMachine.checkHardware(context, diskPath, -1, 0, null, primeRam);
                } else {
                    ParcelFileDescriptor selected = null;
                    String issue = "Android no pudo abrir la ISO.";
                    try {
                        selected = openIso(iso);
                    } catch (IOException error) {
                        if (error.getMessage() != null) issue = error.getMessage();
                    }
                    if (selected == null) {
                        result = NativeMachine.checkHardware(context, diskPath, -1, 0, issue, primeRam);
                    } else {
                        try (ParcelFileDescriptor fd = selected) {
                            result = NativeMachine.checkHardware(context, diskPath, fd.getFd(),
                                    isoSize(iso, fd), null, primeRam);
                        }
                    }
                }
                String json = withBuildMetadata(result);
                send(() -> listener.onHardwareChecked(json));
            } catch (IOException | RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    private ParcelFileDescriptor openIso(Uri iso) throws IOException {
        try {
            ParcelFileDescriptor fd = resolver.openFileDescriptor(iso, "r");
            if (fd == null) throw new IOException("El proveedor devolvió un descriptor vacío.");
            return fd;
        } catch (IOException | SecurityException error) {
            throw new IOException("Android no pudo abrir la ISO. Vuelve a elegir el archivo local.", error);
        }
    }

    private long isoSize(Uri uri, ParcelFileDescriptor fd) {
        long size = fd.getStatSize();
        if (size > 0) return size;
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0))
                return Math.max(0, cursor.getLong(0));
        } catch (RuntimeException ignored) {
            // The native reader also checks the descriptor's size and seekability.
        }
        return 0;
    }

    void start(int source, Uri iso, boolean debug, boolean primeRam) {
        worker.post(() -> {
            if (released || handle != 0) return;
            try {
                debugSelfTest = debug;
                lastLoggedStatus = "";
                lastDiagnostic = "";
                boolean selfTest = source == NativeMachine.BOOT_SELF_TEST;
                String path = selfTest ? "" : disk.getAbsolutePath();
                String dataPath = !selfTest && dataDisk.isFile() ? dataDisk.getAbsolutePath() : "";
                if (source == NativeMachine.BOOT_ISO) {
                    if (iso == null) throw new IOException("Elige primero una imagen ISO.");
                    try (ParcelFileDescriptor fd = openIso(iso)) {
                        int createGiB = disk.exists() ? 0 : DISK_GIB;
                        handle = NativeMachine.open(context, path, createGiB, fd.getFd(),
                                isoSize(iso, fd), source, primeRam, dataPath);
                    }
                } else {
                    handle = NativeMachine.open(context, path, 0, -1, 0, source, primeRam, dataPath);
                }
                if (handle == 0) throw new IllegalStateException("La máquina no se pudo iniciar.");
                outcome = NativeMachine.RUNNING;
                sliceSteps = INITIAL_SLICE_STEPS;
                lastStatus = 0;
                lastDiagnosticSave = 0;
                sessionStarted = SystemClock.elapsedRealtime();
                sessionWorkerCpuStartNs = Debug.threadCpuTimeNanos();
                sessionProcessCpuStartMs = Process.getElapsedCpuTime();
                nextAndroidSample = sessionStarted;
                stepNanos = frameNanos = statusNanos = 0;
                statusSnapshots = diagnosticWrites = diagnosticWriteNanos = 0;
                lastSliceNanos = 0;
                lastSliceSteps = 0;
                lastSliceCompleted = 0;
                videoDirty = true;
                running = true;
                publishState(true);
                reclaimRam = false;
                reclaimPending = false;
                nextRamSample = 0;
                if (foreground) scheduleRam(RAM_SAMPLE_INTERVAL_MS);
                requestFrame();
                schedule(0);
            } catch (IOException | RuntimeException | LinkageError | OutOfMemoryError error) {
                report(error);
            }
        });
    }

    void setForeground(boolean visible) {
        worker.post(() -> {
            foreground = visible;
            if (!visible) {
                worker.removeCallbacks(slice);
                worker.removeCallbacks(ramMaintenance);
            }
            if (running && handle != 0 && !isTerminal()) {
                try {
                    publishState(!visible);
                } catch (RuntimeException | LinkageError error) {
                    report(error);
                }
            }
            if (visible) {
                reclaimRam = false;
                nextRamSample = 0;
                scheduleRam(0);
                requestFrame();
                schedule(0);
            }
        });
    }

    void trimRam(boolean background) {
        worker.post(() -> {
            if ((background && foreground) || released || handle == 0 || isTerminal()) return;
            reclaimRam = background;
            nextRamSample = 0;
            worker.removeCallbacks(ramMaintenance);
            maintainRam();
        });
    }

    void setRunning(boolean resume) {
        worker.post(() -> {
            if (handle == 0 || isTerminal()) return;
            running = resume;
            worker.removeCallbacks(slice);
            if (running) schedule(0);
            try {
                requestFrame();
                publishState(true);
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void key(int ascii, int scan, boolean pressed) {
        worker.post(() -> {
            if (handle == 0 || !running || !foreground || isTerminal()) return;
            try {
                boolean wokeBios = NativeMachine.key(handle, ascii, scan, pressed);
                requestFrame();
                if (outcome == NativeMachine.WAITING_KEY && wokeBios) {
                    outcome = NativeMachine.RUNNING;
                    schedule(0);
                }
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void stop() {
        worker.post(() -> {
            String snapshot = lastDiagnostic;
            if (handle != 0 && outcome != NativeMachine.FAULT) {
                try {
                    snapshot = currentDiagnostic(true);
                } catch (RuntimeException | LinkageError | OutOfMemoryError error) {
                    Log.w(TAG, "No se pudo actualizar el diagnóstico al apagar", error);
                }
            }
            try {
                closeMachine();
                final String diagnostic = snapshot;
                saveDiagnostic(diagnostic);
                send(() -> listener.onState(diagnostic, false, false, false, true));
            } catch (RuntimeException | LinkageError | OutOfMemoryError error) {
                report(error);
            }
        });
    }

    void release() {
        released = true;
        worker.post(() -> {
            try {
                closeMachine();
            } catch (RuntimeException | LinkageError | OutOfMemoryError error) {
                Log.w(TAG, "No se pudo cerrar el volumen QSSD", error);
            } finally {
                thread.quitSafely();
            }
        });
    }

    private void schedule(long delay) {
        worker.removeCallbacks(slice);
        if (!released && foreground && running && handle != 0 &&
                outcome != NativeMachine.WAITING_KEY && !isTerminal()) {
            // Rejoin the ordinary queue after each slice so pending controls can run.
            if (delay == 0) worker.post(slice);
            else worker.postDelayed(slice, delay);
        }
    }

    private void scheduleRam(long delay) {
        worker.removeCallbacks(ramMaintenance);
        if (!released && handle != 0 && !isTerminal())
            worker.postDelayed(ramMaintenance, delay);
    }

    private void maintainRam() {
        if (released || handle == 0 || isTerminal()) return;
        try {
            final long now = SystemClock.uptimeMillis();
            final boolean more;
            if (now >= nextRamSample) {
                more = NativeMachine.refreshRamPressure(context, handle, reclaimRam);
                nextRamSample = now + RAM_SAMPLE_INTERVAL_MS;
            } else {
                more = reclaimPending && NativeMachine.maintainRamBudget(handle);
            }
            reclaimPending = more;
            if (more) {
                if (foreground)
                    nextRamSample = Math.min(nextRamSample, now + RAM_RECLAIM_SAMPLE_INTERVAL_MS);
                scheduleRam(RAM_RECLAIM_INTERVAL_MS);
            } else if (foreground) {
                final boolean preparing = running && NativeMachine.prepareRamReclaim(handle);
                final long untilSample = Math.max(0, nextRamSample - SystemClock.uptimeMillis());
                scheduleRam(preparing ? Math.min(RAM_PREPARE_INTERVAL_MS, untilSample) : untilSample);
            }
        } catch (RuntimeException | LinkageError error) {
            Log.w(TAG, "No se pudo ajustar el respaldo de RAM", error);
            if (foreground) scheduleRam(RAM_SAMPLE_INTERVAL_MS);
        }
    }

    private void runSlice() {
        if (released || !running || !foreground || handle == 0) return;
        try {
            int previous = outcome;
            long started = SystemClock.elapsedRealtimeNanos();
            lastSliceSteps = sliceSteps;
            long step = NativeMachine.step(handle, lastSliceSteps, TARGET_SLICE_NS);
            int updated = (int) step;
            lastSliceCompleted = step >>> 32;
            outcome = updated & ~NativeMachine.STEP_VIDEO_DIRTY;
            videoDirty = (updated & NativeMachine.STEP_VIDEO_DIRTY) != 0;
            long elapsed = Math.max(1, SystemClock.elapsedRealtimeNanos() - started);
            lastSliceNanos = elapsed;
            stepNanos += elapsed;
            // Devices can advance during a slice without retiring a guest instruction.
            if (outcome == NativeMachine.RUNNING && lastSliceCompleted != 0) {
                long estimate = lastSliceCompleted * TARGET_SLICE_NS / elapsed;
                sliceSteps = (int) Math.max(MIN_SLICE_STEPS,
                        Math.min(MAX_SLICE_STEPS, (sliceSteps + estimate) / 2));
            }
            long now = SystemClock.uptimeMillis();
            requestFrame();
            if (isTerminal()) {
                running = false;
                worker.removeCallbacks(ramMaintenance);
                publishState(true);
                return;
            }
            if (now - lastStatus >= STATUS_INTERVAL_MS || outcome != previous)
                publishState(outcome == NativeMachine.WAITING_KEY);
            schedule(outcome == NativeMachine.HALTED ? HALTED_POLL_DELAY_MS : 0);
        } catch (RuntimeException | LinkageError | OutOfMemoryError error) {
            report(error);
        }
    }

    private void requestFrame() {
        if (released || !foreground || handle == 0 || frameRequested || !videoDirty) return;
        long session = handle;
        frameRequested = true;
        if (!main.post(() -> {
            if (released || !foreground || handle != session) {
                worker.post(() -> finishFrame(session, false));
                return;
            }
            Choreographer.getInstance().postFrameCallback(
                    frameTime -> worker.post(() -> captureFrame(session)));
        })) frameRequested = false;
    }

    private void captureFrame(long session) {
        if (released || handle != session) return;
        if (!foreground) {
            finishFrame(session, false);
            return;
        }
        long started = SystemClock.elapsedRealtimeNanos();
        try {
            int[] video = NativeMachine.captureVideo(session, textCells);
            videoDirty = false;
            if (video == null) {
                finishFrame(session, false);
                return;
            }
            Frame frame = new Frame(video, textCells);
            if (!main.post(() -> {
                boolean applied = false;
                try {
                    if (!released && foreground && handle == session) {
                        listener.onFrame(frame);
                        applied = true;
                    }
                } finally {
                    final boolean retry = !applied;
                    worker.post(() -> finishFrame(session, retry));
                }
            })) {
                frameRequested = false;
                NativeMachine.retryVideo(session);
                videoDirty = true;
            }
        } catch (RuntimeException | LinkageError error) {
            frameRequested = false;
            report(error);
        } finally {
            frameNanos += SystemClock.elapsedRealtimeNanos() - started;
        }
    }

    private void finishFrame(long session, boolean retry) {
        if (released || handle != session) return;
        frameRequested = false;
        try {
            if (retry) {
                NativeMachine.retryVideo(session);
                videoDirty = true;
            }
            requestFrame();
        } catch (RuntimeException | LinkageError error) {
            report(error);
        }
    }

    private String currentDiagnostic(boolean detailed) {
        long now = SystemClock.elapsedRealtime();
        if (now >= nextAndroidSample) {
            androidPssKiB = Debug.getPss();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && powerManager != null)
                androidThermalStatus = powerManager.getCurrentThermalStatus();
            nextAndroidSample = now + ANDROID_SAMPLE_INTERVAL_MS;
        }
        long workerCpuNs = Debug.threadCpuTimeNanos();
        String workerCpuUs = sessionWorkerCpuStartNs >= 0 && workerCpuNs >= sessionWorkerCpuStartNs
                ? Long.toString((workerCpuNs - sessionWorkerCpuStartNs) / 1000) : "null";
        long processCpuMs = Process.getElapsedCpuTime() - sessionProcessCpuStartMs;
        ++statusSnapshots;
        return withBuildMetadata(NativeMachine.diagnostic(handle, detailed),
                ",\"elapsed_ms\":" + (now - sessionStarted)
                + ",\"android_pss_kib\":" + androidPssKiB
                + ",\"android_thermal_status\":" + (androidThermalStatus >= 0
                        ? Integer.toString(androidThermalStatus) : "null")
                + ",\"worker_timing\":{\"step_us\":" + stepNanos / 1000
                + ",\"host_thread_cpu_us\":" + workerCpuUs
                + ",\"host_process_cpu_ms\":" + processCpuMs
                + ",\"frame_us\":" + frameNanos / 1000
                + ",\"diagnostic_us\":" + statusNanos / 1000
                + ",\"diagnostic_snapshots\":" + statusSnapshots
                + ",\"diagnostic_writes\":" + diagnosticWrites
                + ",\"diagnostic_write_us\":" + diagnosticWriteNanos / 1000
                + ",\"last_slice_steps_requested\":" + lastSliceSteps
                + ",\"last_slice_steps_completed\":" + lastSliceCompleted
                + ",\"last_slice_us\":" + lastSliceNanos / 1000
                + ",\"next_slice_steps_requested\":" + sliceSteps
                + ",\"max_steps_per_slice\":" + MAX_SLICE_STEPS
                + ",\"target_slice_us\":" + TARGET_SLICE_NS / 1000
                + ",\"diagnostic_save_interval_ms\":" + DIAGNOSTIC_INTERVAL_MS + "}");
    }

    private void publishState(boolean saveNow) {
        long started = SystemClock.elapsedRealtimeNanos();
        lastStatus = SystemClock.uptimeMillis();
        boolean detailed = debugSelfTest || saveNow || lastStatus - lastDiagnosticSave >= DIAGNOSTIC_INTERVAL_MS;
        String json = currentDiagnostic(detailed);
        if (detailed) {
            lastDiagnostic = json;
        }
        boolean stopped = outcome == NativeMachine.STOPPED;
        boolean active = running && foreground && !isTerminal();
        if (debugSelfTest) {
            String state = Integer.toString(outcome);
            if (!state.equals(lastLoggedStatus)) {
                Log.i(TAG, json);
                lastLoggedStatus = state;
            }
        }
        send(() -> listener.onState(json, active, true, stopped, detailed));
        if (saveNow || lastStatus - lastDiagnosticSave >= DIAGNOSTIC_INTERVAL_MS)
            saveDiagnostic(json);
        statusNanos += SystemClock.elapsedRealtimeNanos() - started;
    }

    private static String withBuildMetadata(String json) {
        return withBuildMetadata(json, "");
    }

    private static String withBuildMetadata(String json, String extraFields) {
        if (json == null || !json.startsWith("{\"")) {
            throw new IllegalStateException("El diagnóstico de la máquina virtual no es válido.");
        }
        // Preserve the native JSON's full 64-bit register values.
        return "{" + BUILD_METADATA + "," + json.substring(1, json.length() - 1) + extraFields + "}";
    }

    private void saveDiagnostic(String json) {
        if (json.isEmpty() || json.equals(persistedDiagnostic)) return;
        lastDiagnosticSave = SystemClock.uptimeMillis();
        long started = SystemClock.elapsedRealtimeNanos();
        FileOutputStream stream = null;
        try {
            stream = diagnosticFile.startWrite();
            stream.write(json.getBytes(StandardCharsets.UTF_8));
            diagnosticFile.finishWrite(stream);
            ++diagnosticWrites;
            persistedDiagnostic = json;
        } catch (IOException error) {
            if (stream != null) diagnosticFile.failWrite(stream);
            Log.w(TAG, "No se pudo conservar el último diagnóstico", error);
        } finally {
            diagnosticWriteNanos += SystemClock.elapsedRealtimeNanos() - started;
        }
    }

    private boolean isTerminal() {
        return outcome == NativeMachine.FAULT || outcome == NativeMachine.STOPPED;
    }

    private void closeMachine() {
        worker.removeCallbacks(slice);
        worker.removeCallbacks(ramMaintenance);
        running = false;
        frameRequested = false;
        videoDirty = false;
        if (handle == 0) return;
        long current = handle;
        handle = 0;
        try {
            NativeMachine.flush(current);
        } finally {
            NativeMachine.close(current);
        }
    }

    private void report(Throwable error) {
        worker.removeCallbacks(slice);
        worker.removeCallbacks(ramMaintenance);
        running = false;
        String detail = error.getMessage();
        String message = detail == null || detail.isEmpty() ? error.getClass().getSimpleName() : detail;
        boolean hasSession = handle != 0;
        if (hasSession) outcome = NativeMachine.FAULT;
        String snapshot = hasSession ? lastDiagnostic : "";
        try {
            if (hasSession && !(error instanceof OutOfMemoryError)) snapshot = currentDiagnostic(true);
        } catch (RuntimeException | LinkageError | OutOfMemoryError ignored) {
            // Retain the most recent successful native snapshot.
        }
        lastDiagnostic = withBuildMetadata("{\"schema\":\"reverse-windows-android-error-v1\","
                + "\"status\":\"fault\",\"message\":" + JSONObject.quote(message)
                + ",\"last_machine_diagnostic\":" + (snapshot.isEmpty() ? "null" : snapshot) + "}");
        saveDiagnostic(lastDiagnostic);
        String diagnostic = lastDiagnostic;
        send(() -> listener.onError(message, hasSession));
        send(() -> listener.onSavedDiagnostic(diagnostic));
    }

    private void send(Runnable action) {
        main.post(() -> {
            if (!released) action.run();
        });
    }
}
