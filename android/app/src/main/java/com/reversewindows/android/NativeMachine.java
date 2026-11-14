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
        // Scope the anonymous spill file to ordinary application-private storage.
        return cache.getAbsolutePath();
    }

    static String checkHardware(Context context, String qssdPath, int isoFd, long isoBytes,
                                String isoError) {
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        return checkHardwareWithRam(qssdPath, isoFd, isoBytes, isoError,
                memory == null ? -1 : memory.availMem, memory == null ? 0 : memory.threshold,
                memory != null && memory.lowMemory, ramCache(context));
    }

    static long open(Context context, String qssdPath, int capacityGiB, int isoFd, long isoBytes,
                     int bootSource) {
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        return openWithRam(qssdPath, capacityGiB, isoFd, isoBytes, bootSource,
                memory == null ? -1 : memory.availMem, memory == null ? 0 : memory.threshold,
                memory != null && memory.lowMemory, ramCache(context));
    }

    static boolean refreshRamPressure(Context context, long handle, boolean reclaimMemory) {
        ActivityManager.MemoryInfo memory = memoryInfo(context);
        if (memory == null && !reclaimMemory) return false;
        return sampleRamPressure(handle, memory == null ? 0 : memory.availMem,
                memory == null ? 0 : memory.threshold, reclaimMemory || memory.lowMemory);
    }

    static native void createDisk(String qssdPath, int capacityGiB);
    private static native String checkHardwareWithRam(String qssdPath, int isoFd, long isoBytes,
                                                      String isoError, long available, long threshold,
                                                      boolean lowMemory, String ramCacheDirectory);
    private static native long openWithRam(String qssdPath, int capacityGiB, int isoFd, long isoBytes,
                                           int bootSource, long available, long threshold,
                                           boolean lowMemory, String ramCacheDirectory);
    static native void close(long handle);
    static native int step(long handle, int instructions);
    private static native boolean sampleRamPressure(long handle, long available, long threshold,
                                                    boolean lowMemory);
    static native boolean maintainRamBudget(long handle);
    static native boolean key(long handle, int ascii, int scanCode, boolean pressed);
    static native void flush(long handle);
    static native String diagnostic(long handle);
    static native int[] videoInfo(long handle);
    static native int copyText(long handle, byte[] cells);
    static native void copyGraphics(long handle, int width, int height, int[] argb);
}
