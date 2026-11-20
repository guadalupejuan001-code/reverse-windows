package com.reversewindows.android;

/** Minimum host rest per four nanoseconds spent in a running guest slice. */
enum VmHostPacing {
    NORMAL(0), MODERATE(1), SEVERE(4), CRITICAL(8);

    private final int restQuarters;

    VmHostPacing(int restQuarters) {
        this.restQuarters = restQuarters;
    }

    long delayMillis(long engineWaitNs, long boundedWorkNs, boolean progressed) {
        long restNs = boundedWorkNs * restQuarters / 4;
        if (!progressed) restNs = Math.max(restNs, 1_000_000L);
        // The engine's pacing interval already gives Android time for other work.
        return (Math.max(engineWaitNs, restNs) + 999_999L) / 1_000_000L;
    }
}
