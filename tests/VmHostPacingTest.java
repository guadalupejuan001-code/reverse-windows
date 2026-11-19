package com.reversewindows.android;

public final class VmHostPacingTest {
    private static void check(long expected, VmHostPacing mode, long engineWait, long work, boolean progressed) {
        long actual = mode.delayMillis(engineWait, work, progressed);
        if (actual != expected) throw new AssertionError(mode + " expected " + expected + " ms, got " + actual);
    }

    public static void main(String[] args) {
        check(0, VmHostPacing.NORMAL, 0, 8_000_000L, true);
        check(1, VmHostPacing.NORMAL, 0, 100L, false);
        check(2, VmHostPacing.MODERATE, 0, 8_000_000L, true);
        check(8, VmHostPacing.SEVERE, 0, 8_000_000L, true);
        check(16, VmHostPacing.CRITICAL, 0, 8_000_000L, true);
        check(6, VmHostPacing.SEVERE, 5_500_000L, 2_000_000L, true);
        check(4, VmHostPacing.SEVERE, 500_000L, 4_000_000L, true);
        check(1, VmHostPacing.MODERATE, 0, 2_100_000L, true);
        System.out.println("VM_HOST_PACING_PASS scenarios=8");
    }
}
