package com.reversewindows.android;

public final class VmRamMaintenanceTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void crossingBetweenAndroidSamples(long needsPreparationAt, String scenario) {
        final long androidSampleAt = 10_000;
        long now = 0;
        int checks = 0;
        // At t=0 the resident threshold has not been reached, or the reserve is full.
        // Guest execution changes that condition just after an eligibility check.
        do {
            now += VmRamMaintenance.delayMillis(true, false, androidSampleAt - now);
            checks++;
            require(now < androidSampleAt, scenario + " waited for the Android sample");
        } while (now < needsPreparationAt);
        require(now - needsPreparationAt < 250, scenario + " missed the eligibility window");
        require(checks <= (now + 249) / 250, "Idle checks must be bounded to four per second");
        require(VmRamMaintenance.delayMillis(true, true, androidSampleAt - now) == 50,
                "Eligible preparation must retain its existing batch cadence");
    }

    public static void main(String[] args) {
        crossingBetweenAndroidSamples(1, "Initial threshold crossing");
        crossingBetweenAndroidSamples(2_001, "Guest invalidates prepared copies");
        require(VmRamMaintenance.delayMillis(true, false, 24) == 24,
                "Eligibility checks must not delay an earlier Android sample");
        require(VmRamMaintenance.delayMillis(true, true, 24) == 24,
                "Preparation batches must not delay an earlier Android sample");
        require(VmRamMaintenance.delayMillis(false, false, 10_000) == 10_000,
                "Paused guests must not poll preparation");
        require(VmRamMaintenance.delayMillis(false, true, 10_000) == 10_000,
                "A stale preparing flag must not poll while paused");
        require(VmRamMaintenance.delayMillis(true, false, -1) == 0,
                "An overdue Android sample must run immediately");
        System.out.println("VM_RAM_MAINTENANCE_PASS threshold=1 invalidation=1 paused=1 sample_deadline=1");
    }
}
