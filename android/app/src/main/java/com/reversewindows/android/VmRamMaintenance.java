package com.reversewindows.android;

/** Scheduling only: Android pressure sampling remains independent of preparation checks. */
final class VmRamMaintenance {
    private static final long PREPARE_INTERVAL_MS = 50;
    // A completed reserve or a below-threshold check can become stale as the guest runs.
    // Recheck idle eligibility every 250 ms, unless an Android sample is due sooner.
    private static final long ELIGIBILITY_INTERVAL_MS = 250;

    private VmRamMaintenance() {}

    static long delayMillis(boolean running, boolean preparing, long untilSample) {
        long sampleDelay = Math.max(0, untilSample);
        if (!running) return sampleDelay;
        return Math.min(sampleDelay, preparing ? PREPARE_INTERVAL_MS : ELIGIBILITY_INTERVAL_MS);
    }
}
