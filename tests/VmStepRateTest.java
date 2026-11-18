package com.reversewindows.android;

public final class VmStepRateTest {
    private static void check(long expected, long actual, String scenario) {
        if (expected != actual) {
            throw new AssertionError(scenario + " expected " + expected + " got " + actual);
        }
    }

    public static void main(String[] args) {
        VmStepRate rate = new VmStepRate();
        check(-1, rate.sample(true, 1000, 1000), "first sample");
        check(500_000, rate.sample(true, 2000, 3000), "execution window");
        check(-1, rate.sample(true, 2000, 3000), "duplicate sample");
        check(0, rate.sample(true, 2000, 3500), "no guest progress");
        check(-1, rate.sample(false, 2000, 3500), "paused or waiting");
        check(-1, rate.sample(true, 9000, 6000), "new baseline after waiting");
        check(4_000_000, rate.sample(true, 10000, 6250), "resumed execution");
        check(-1, rate.sample(true, 1, 6300), "counter reset");
        check(-1, rate.sample(true, 2, -1), "missing timing");
        check(-1, rate.sample(true, 3, 6350), "baseline after missing timing");
        check(20_000, rate.sample(true, 4, 6400), "valid again");
        System.out.println("VM_STEP_RATE_PASS scenarios=11");
    }
}
