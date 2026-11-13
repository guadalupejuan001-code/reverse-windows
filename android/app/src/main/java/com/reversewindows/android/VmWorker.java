package com.reversewindows.android;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.util.AtomicFile;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

final class VmWorker {
    interface Listener {
        void onState(String json, boolean running, boolean hasSession, boolean stopped);
        void onFrame(Frame frame);
        void onError(String message, boolean hasSession);
        void onDiskCreated();
        void onHardwareChecked(String json);
        void onSavedDiagnostic(String json);
        void onDiagnosticExported(boolean saved);
    }

    static final class Frame {
        final int width;
        final int height;
        final byte[] text;
        final int[] pixels;

        Frame(int width, int height, byte[] text, int[] pixels) {
            this.width = width;
            this.height = height;
            this.text = text;
            this.pixels = pixels;
        }
    }

    private static final String TAG = "ReverseWindows";
    private static final String BUILD_METADATA =
            "\"app_version\":" + JSONObject.quote(BuildConfig.VERSION_NAME) +
            ",\"app_version_code\":" + BuildConfig.VERSION_CODE +
            ",\"application_id\":" + JSONObject.quote(BuildConfig.APPLICATION_ID) +
            ",\"mundo_x_sha\":" + JSONObject.quote(BuildConfig.MUNDO_X_SHA);
    private static final int DISK_GIB = 64;
    private static final int INITIAL_SLICE_STEPS = 4096;
    private static final int MIN_SLICE_STEPS = 64;
    private static final int MAX_SLICE_STEPS = 16384;
    private static final long TARGET_SLICE_NS = 8_000_000L;
    private static final long HALTED_POLL_DELAY_MS = 35;
    private static final long FRAME_INTERVAL_MS = 100;
    private static final long STATUS_INTERVAL_MS = 350;
    private static final long DIAGNOSTIC_INTERVAL_MS = 5_000;
    private static final String DIAGNOSTIC_FILE = "ultimo-diagnostico.json";
    private static final int MAX_FRAME_WIDTH = 960;
    private static final int MAX_FRAME_HEIGHT = 720;

    private final Context context;
    private final ContentResolver resolver;
    private final File disk;
    private final AtomicFile diagnosticFile;
    private final Listener listener;
    private final HandlerThread thread = new HandlerThread("reverse-windows-vm");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean framePending = new AtomicBoolean();
    private final byte[] textCells = new byte[4000];

    private volatile boolean released;
    private long handle;
    private int outcome;
    private int sliceSteps = INITIAL_SLICE_STEPS;
    private boolean running;
    private boolean foreground = true;
    private boolean debugSelfTest;
    private long lastFrame;
    private long lastStatus;
    private long lastDiagnosticSave;
    private long sessionStarted;
    private int[] graphics;
    private int graphicsWidth;
    private int graphicsHeight;
    private boolean forceFrame;
    private boolean retryFrame;
    private String lastLoggedStatus = "";
    private String lastDiagnostic = "";
    private String persistedDiagnostic = "";
    private final Runnable slice = this::runSlice;

    VmWorker(Context context, File disk, Listener listener) {
        this.context = context.getApplicationContext();
        this.resolver = this.context.getContentResolver();
        this.disk = disk;
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

    void createDisk() {
        worker.post(() -> {
            try {
                NativeMachine.createDisk(disk.getAbsolutePath(), DISK_GIB);
                send(listener::onDiskCreated);
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void checkHardware(Uri iso) {
        worker.post(() -> {
            if (released || handle != 0) return;
            try {
                String diskPath = disk.exists() ? disk.getAbsolutePath() : "";
                String result;
                if (iso == null) {
                    result = NativeMachine.checkHardware(context, diskPath, -1, 0, null);
                } else {
                    ParcelFileDescriptor selected = null;
                    String issue = "Android no pudo abrir la ISO.";
                    try {
                        selected = resolver.openFileDescriptor(iso, "r");
                    } catch (IOException | SecurityException error) {
                        if (error.getMessage() != null) issue = error.getMessage();
                    }
                    if (selected == null) {
                        result = NativeMachine.checkHardware(context, diskPath, -1, 0, issue);
                    } else {
                        try (ParcelFileDescriptor fd = selected) {
                            result = NativeMachine.checkHardware(context, diskPath, fd.getFd(),
                                    Math.max(0, fd.getStatSize()), null);
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

    void start(int source, Uri iso, boolean debug) {
        worker.post(() -> {
            if (handle != 0) return;
            try {
                debugSelfTest = debug;
                lastLoggedStatus = "";
                lastDiagnostic = "";
                retryFrame = false;
                String path = source == NativeMachine.BOOT_SELF_TEST ? "" : disk.getAbsolutePath();
                if (source == NativeMachine.BOOT_ISO) {
                    if (iso == null) throw new IOException("Elige primero una imagen ISO.");
                    try (ParcelFileDescriptor fd = resolver.openFileDescriptor(iso, "r")) {
                        if (fd == null) throw new IOException("Android no pudo abrir la ISO.");
                        handle = NativeMachine.open(context, path, 0, fd.getFd(),
                                Math.max(0, fd.getStatSize()), source);
                    }
                } else {
                    handle = NativeMachine.open(context, path, 0, -1, 0, source);
                }
                if (handle == 0) throw new IllegalStateException("La máquina no se pudo iniciar.");
                outcome = NativeMachine.RUNNING;
                sliceSteps = INITIAL_SLICE_STEPS;
                lastStatus = 0;
                lastDiagnosticSave = 0;
                sessionStarted = SystemClock.elapsedRealtime();
                lastFrame = 0;
                forceFrame = true;
                running = true;
                publishState(true);
                schedule(0);
            } catch (IOException | RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void setForeground(boolean visible) {
        worker.post(() -> {
            foreground = visible;
            if (visible) schedule(0);
            else {
                worker.removeCallbacks(slice);
                if (running && handle != 0) {
                    try {
                        publishState(true);
                    } catch (RuntimeException | LinkageError error) {
                        report(error);
                    }
                }
            }
        });
    }

    void setRunning(boolean resume) {
        worker.post(() -> {
            if (handle == 0 || isTerminal()) return;
            running = resume;
            worker.removeCallbacks(slice);
            if (running) schedule(0);
            try {
                publishState(true);
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void key(int ascii, int scan, boolean pressed) {
        worker.post(() -> {
            if (handle == 0 || !running || isTerminal()) return;
            try {
                NativeMachine.key(handle, ascii, scan, pressed);
                if (outcome == NativeMachine.WAITING_KEY && pressed) {
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
            try {
                String diagnostic = handle == 0 ? "" : currentDiagnostic();
                saveDiagnostic(diagnostic);
                closeMachine();
                send(() -> listener.onState(diagnostic, false, false, false));
            } catch (RuntimeException | LinkageError error) {
                report(error);
            }
        });
    }

    void release() {
        released = true;
        worker.post(() -> {
            try {
                closeMachine();
            } catch (RuntimeException | LinkageError error) {
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

    private void runSlice() {
        if (released || !running || !foreground || handle == 0) return;
        try {
            int previous = outcome;
            long started = SystemClock.elapsedRealtimeNanos();
            outcome = NativeMachine.step(handle, sliceSteps);
            if (outcome == NativeMachine.RUNNING) {
                long elapsed = Math.max(1, SystemClock.elapsedRealtimeNanos() - started);
                long estimate = sliceSteps * TARGET_SLICE_NS / elapsed;
                sliceSteps = (int) Math.max(MIN_SLICE_STEPS,
                        Math.min(MAX_SLICE_STEPS, (sliceSteps + estimate) / 2));
            }
            long now = SystemClock.uptimeMillis();
            if (now - lastFrame >= FRAME_INTERVAL_MS || outcome != previous || forceFrame) {
                publishFrame();
                lastFrame = now;
            }
            if (now - lastStatus >= STATUS_INTERVAL_MS || outcome != previous)
                publishState(isTerminal());
            if (isTerminal()) running = false;
            schedule(outcome == NativeMachine.HALTED ? HALTED_POLL_DELAY_MS : 0);
        } catch (RuntimeException | LinkageError error) {
            report(error);
        }
    }

    private void publishFrame() {
        if (!framePending.compareAndSet(false, true)) {
            retryFrame = true;
            return;
        }
        retryFrame = false;
        boolean posted = false;
        try {
            int[] info = NativeMachine.videoInfo(handle);
            if (info == null || info.length != 3 || info[0] <= 0 || info[1] <= 0) return;
            Frame frame;
            if (info[2] == 0) {
                int copied = NativeMachine.copyText(handle, textCells);
                if (copied == 0 && !forceFrame) return;
                frame = new Frame(info[0], info[1], textCells, null);
            } else {
                double scale = Math.min(1.0, Math.min((double) MAX_FRAME_WIDTH / info[0],
                        (double) MAX_FRAME_HEIGHT / info[1]));
                int width = Math.max(1, (int) (info[0] * scale));
                int height = Math.max(1, (int) (info[1] * scale));
                if (graphics == null || graphicsWidth != width || graphicsHeight != height) {
                    graphics = new int[width * height];
                    graphicsWidth = width;
                    graphicsHeight = height;
                }
                NativeMachine.copyGraphics(handle, width, height, graphics);
                frame = new Frame(width, height, null, graphics);
            }
            forceFrame = false;
            posted = main.post(() -> {
                try {
                    if (!released) listener.onFrame(frame);
                } finally {
                    framePending.set(false);
                    // The final frame may have changed while Android was presenting the prior one.
                    worker.post(() -> {
                        if (retryFrame && handle != 0) publishFrame();
                    });
                }
            });
        } finally {
            if (!posted) framePending.set(false);
        }
    }

    private String currentDiagnostic() {
        return withBuildMetadata(NativeMachine.diagnostic(handle),
                SystemClock.elapsedRealtime() - sessionStarted);
    }

    private void publishState(boolean saveNow) {
        lastStatus = SystemClock.uptimeMillis();
        String json = currentDiagnostic();
        lastDiagnostic = json;
        if (saveNow || lastStatus - lastDiagnosticSave >= DIAGNOSTIC_INTERVAL_MS)
            saveDiagnostic(json);
        boolean stopped = outcome == NativeMachine.STOPPED;
        boolean active = running && foreground && !isTerminal();
        if (debugSelfTest) {
            String state = Integer.toString(outcome);
            if (!state.equals(lastLoggedStatus)) {
                Log.i(TAG, json);
                lastLoggedStatus = state;
            }
        }
        send(() -> listener.onState(json, active, true, stopped));
    }

    private static String withBuildMetadata(String json) {
        return withBuildMetadata(json, -1);
    }

    private static String withBuildMetadata(String json, long elapsedMs) {
        if (json == null || !json.startsWith("{\"")) {
            throw new IllegalStateException("El diagnóstico de la máquina virtual no es válido.");
        }
        // Preserve the native JSON's full 64-bit register values.
        String tail = elapsedMs >= 0 ? ",\"elapsed_ms\":" + elapsedMs + "}" : "}";
        return "{" + BUILD_METADATA + "," + json.substring(1, json.length() - 1) + tail;
    }

    private void saveDiagnostic(String json) {
        if (json.isEmpty() || json.equals(persistedDiagnostic)) return;
        lastDiagnosticSave = SystemClock.uptimeMillis();
        FileOutputStream stream = null;
        try {
            stream = diagnosticFile.startWrite();
            stream.write(json.getBytes(StandardCharsets.UTF_8));
            diagnosticFile.finishWrite(stream);
            persistedDiagnostic = json;
        } catch (IOException error) {
            if (stream != null) diagnosticFile.failWrite(stream);
            Log.w(TAG, "No se pudo conservar el último diagnóstico", error);
        }
    }

    private boolean isTerminal() {
        return outcome == NativeMachine.FAULT || outcome == NativeMachine.STOPPED;
    }

    private void closeMachine() {
        worker.removeCallbacks(slice);
        running = false;
        retryFrame = false;
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
        running = false;
        String detail = error.getMessage();
        String message = detail == null || detail.isEmpty() ? error.getClass().getSimpleName() : detail;
        boolean hasSession = handle != 0;
        if (hasSession) outcome = NativeMachine.FAULT;
        String snapshot = hasSession ? lastDiagnostic : "";
        try {
            if (hasSession) snapshot = currentDiagnostic();
        } catch (RuntimeException | LinkageError ignored) {
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
