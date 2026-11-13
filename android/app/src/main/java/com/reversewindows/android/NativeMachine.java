package com.reversewindows.android;

final class NativeMachine {
    static final int BOOT_ISO = 1;
    static final int BOOT_QSSD = 2;
    static final int BOOT_SELF_TEST = 3;
    static final int WAITING_KEY = 1;
    static final int HALTED = 2;
    static final int FAULT = 3;
    static final int STOPPED = 4;

    static {
        System.loadLibrary("reverse_windows_android");
    }

    private NativeMachine() {}

    static native void createDisk(String qssdPath, int capacityGiB);
    static native long open(String qssdPath, int capacityGiB, int isoFd, long isoBytes, int bootSource);
    static native void close(long handle);
    static native int step(long handle, int instructions);
    static native void key(long handle, int ascii, int scanCode, boolean pressed);
    static native void flush(long handle);
    static native String diagnostic(long handle);
    static native int[] videoInfo(long handle);
    static native int copyText(long handle, byte[] cells);
    static native void copyGraphics(long handle, int width, int height, int[] argb);
}
