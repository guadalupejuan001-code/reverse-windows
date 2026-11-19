package com.reversewindows.android;

import android.app.ActivityManager;
import android.content.Context;

import java.io.File;

final class NativeMachine {
    static final int BOOT_ISO = 1;
    static final int BOOT_QSSD = 2;
    static final int BOOT_SELF_TEST = 3;
    static final int RUNNING = 0;
    static final int WAITING_KEY = 1;
    static final int HALTED = 2;
    static final int FAULT = 3;
    static final int STOPPED = 4;
    static final int STEP_VIDEO_DIRTY = 0x100;

    static {
        System.loadLibrary("reverse_windows_android");
    }

    private NativeMachine() {}

    private static ActivityManager.MemoryInfo memoryInfo(Context context) {
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        if (manager == null) return null;
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(info);
        return info;
    }

    private static String ramCache(Context context) {
        File cache = context.getCacheDir();
        if (cache == null) return "";
        // The temporary QSSD pages live in application-private storage.
        return cache.getAbsolutePath();
    }

    static String checkHardware(Context context, String qssdPath, int isoFd, long isoBytes,
                                String isoError, boolean primeRam) {
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        return checkHardwareWithRam(qssdPath, isoFd, isoBytes, isoError,
                memory == null ? -1 : memory.availMem, memory == null ? 0 : memory.threshold,
                memory != null && memory.lowMemory, primeRam, ramCache(context));
    }

    static long open(Context context, String qssdPath, int capacityGiB, int isoFd, long isoBytes,
                     int bootSource, boolean primeRam, String dataDiskPath) {
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        File graphicsVram = new File(context.getFilesDir(), "gpu-vram.rqvd");
        if (!graphicsVram.exists() && qssdPath != null && !qssdPath.isEmpty()) {
            File primeVgcVram = new File(qssdPath + ".vram.rqvd");
            if (primeVgcVram.isFile()) graphicsVram = primeVgcVram;
        }
        return openWithRam(qssdPath, capacityGiB, isoFd, isoBytes, bootSource,
                memory == null ? -1 : memory.availMem, memory == null ? 0 : memory.threshold,
                memory != null && memory.lowMemory, primeRam, ramCache(context), graphicsVram.getAbsolutePath(),
                dataDiskPath);
    }

    static boolean refreshRamPressure(Context context, long handle, boolean reclaimMemory) {
        if (reclaimMemory) return sampleRamPressure(handle, 0, 0, true);
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        return memory != null && sampleRamPressure(handle, memory.availMem,
                memory.threshold, memory.lowMemory);
    }

    static native void createDisk(String qssdPath, int capacityGiB);
    private static native String checkHardwareWithRam(String qssdPath, int isoFd, long isoBytes,
                                                      String isoError, long available, long threshold,
                                                      boolean lowMemory, boolean primeRam,
                                                      String ramCacheDirectory);
    private static native long openWithRam(String qssdPath, int capacityGiB, int isoFd, long isoBytes,
                                           int bootSource, long available, long threshold,
                                           boolean lowMemory, boolean primeRam,
                                           String ramCacheDirectory, String graphicsVram, String dataDiskPath);
    static native void close(long handle);
    // Low 32 bits contain the result flags. High 32 bits count retired guest steps.
    static native long step(long handle, int instructions, long hostBudgetNs);
    static native void setExperimentalClock(long handle, boolean enabled);
    static native int[] cpuThreadIds(long handle);
    private static native boolean sampleRamPressure(long handle, long available, long threshold,
                                                    boolean lowMemory);
    static native boolean maintainRamBudget(long handle);
    static native boolean prepareRamReclaim(long handle);
    static native boolean key(long handle, int ascii, int scanCode, boolean pressed);
    static native void flush(long handle);
    static native String diagnostic(long handle, boolean detailed);
    static native int[] captureVideo(long handle, byte[] text);
    static native void retryVideo(long handle);
}
