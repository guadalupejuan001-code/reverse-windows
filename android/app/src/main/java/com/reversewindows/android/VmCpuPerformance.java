package com.reversewindows.android;

import android.annotation.TargetApi;
import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.os.PowerManager;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.Arrays;

/** Accessed on the VM thread. Android selects the physical CPU operating point. */
final class VmCpuPerformance {
    private final PowerManager power;
    private final Hints hints;
    private long nextSampleMs;
    private long nextThreadCheckMs;
    private int thermal = -1;
    private long lowKhz;
    private long highKhz;
    private int policies;

    VmCpuPerformance(Context context, long targetNs) {
        power = context.getSystemService(PowerManager.class);
        hints = Build.VERSION.SDK_INT >= 31 ? new Hints(context, targetNs) : null;
    }

    void sample(long now) {
        if (now < nextSampleMs) return;
        nextSampleMs = now + 1000;
        if (power != null && Build.VERSION.SDK_INT >= 29) {
            try {
                thermal = power.getCurrentThermalStatus();
            } catch (RuntimeException unavailable) {
                thermal = -1;
            }
        }
        if (hints != null && !thermalAllowsHint()) hints.close();
        lowKhz = highKhz = 0;
        policies = 0;
        File root = new File("/sys/devices/system/cpu/cpufreq");
        try {
            File[] entries = root.listFiles((dir, name) -> name.startsWith("policy"));
            if (entries == null) return;
            for (File policy : entries) {
                long khz = readKhz(new File(policy, "cpuinfo_cur_freq"));
                if (khz <= 0) khz = readKhz(new File(policy, "scaling_cur_freq"));
                if (khz <= 0) continue;
                lowKhz = policies == 0 ? khz : Math.min(lowKhz, khz);
                highKhz = Math.max(highKhz, khz);
                ++policies;
            }
        } catch (SecurityException unavailable) {
            // Some Android kernels expose no per-policy frequency to apps.
        }
    }

    private static long readKhz(File file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            return Long.parseLong(reader.readLine());
        } catch (IOException | SecurityException | NumberFormatException unavailable) {
            return 0;
        }
    }

    private boolean thermalAllowsHint() {
        return thermal >= 0 && thermal < PowerManager.THERMAL_STATUS_MODERATE;
    }

    void beforeWork(long handle) {
        long now = SystemClock.elapsedRealtime();
        sample(now);
        if (hints == null || !thermalAllowsHint() || now < nextThreadCheckMs) return;
        boolean attached = hints.attach(handle);
        nextThreadCheckMs = now + (attached ? 1000 : 10000);
    }

    void report(long elapsedNs) {
        if (hints != null) hints.report(elapsedNs);
    }

    void pause() {
        if (hints != null) hints.close();
        nextThreadCheckMs = 0;
    }

    String diagnosticFields() {
        return ",\"android_thermal_status\":" + (thermal < 0 ? "null" : Integer.toString(thermal))
                + ",\"host_cpu\":{\"readable_policies\":" + policies
                + ",\"min_reported_khz\":" + (policies == 0 ? "null" : Long.toString(lowKhz))
                + ",\"max_reported_khz\":" + (policies == 0 ? "null" : Long.toString(highKhz))
                + ",\"hint_threads\":" + (hints == null ? 0 : hints.tids.length) + "}";
    }

    @TargetApi(31)
    private static final class Hints {
        private static final int[] EMPTY = {};
        private final PerformanceHintManager manager;
        private final long targetNs;
        private PerformanceHintManager.Session session;
        private int[] tids = EMPTY;

        Hints(Context context, long targetNs) {
            manager = context.getSystemService(PerformanceHintManager.class);
            this.targetNs = targetNs;
        }

        boolean attach(long handle) {
            if (manager == null) return false;
            try {
                int[] current = NativeMachine.cpuThreadIds(handle);
                if (session != null && Arrays.equals(tids, current)) return true;
                close();
                if (current == null || current.length == 0) return false;
                session = manager.createHintSession(current, targetNs);
                if (session == null) return false;
                tids = current;
                return true;
            } catch (RuntimeException unavailable) {
                close();
                return false;
            }
        }

        void report(long elapsedNs) {
            if (session == null) return;
            try {
                session.reportActualWorkDuration(elapsedNs);
            } catch (RuntimeException unavailable) {
                close();
            }
        }

        void close() {
            if (session != null) {
                try {
                    session.close();
                } catch (RuntimeException unavailable) {
                    // Android may already have retired the foreground session.
                }
            }
            session = null;
            tids = EMPTY;
        }
    }
}
