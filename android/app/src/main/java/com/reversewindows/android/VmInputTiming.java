package com.reversewindows.android;

/** Bounded timestamps and counts, with no touch coordinates or key values. */
final class VmInputTiming {
    private final long[] touchDowns = new long[8];
    private final Sample[] samples = new Sample[32];
    private long sessionStartUs;
    private long touchDownCount;
    private int sampleCount;
    private int nextSample;
    private long guestPointerCount;
    private long guestKeyDownCount;

    private record Sample(long elapsedUs, long retiredSteps, long engineUs, boolean executing,
                          long touchDownCount, long guestPointerCount, long guestKeyDownCount,
                          long hostSampleUs, long hostMinKhz, long hostMaxKhz,
                          int thermalStatus, int hintThreads, long[] inputIrqs) {}

    void begin(long startUs) {
        synchronized (touchDowns) {
            sessionStartUs = startUs;
            touchDownCount = 0;
        }
        sampleCount = nextSample = 0;
        guestPointerCount = guestKeyDownCount = 0;
    }

    // Only the touch ring is shared with Android's UI thread.
    void androidTouchDown(long elapsedUs) {
        synchronized (touchDowns) {
            if (sessionStartUs == 0 || elapsedUs < sessionStartUs) return;
            touchDowns[(int) (touchDownCount % touchDowns.length)] = elapsedUs;
            ++touchDownCount;
        }
    }

    void guestPointerForwarded() { ++guestPointerCount; }
    void guestKeyDownForwarded() { ++guestKeyDownCount; }

    void sample(long elapsedUs, long retiredSteps, long engineUs, boolean executing,
                VmCpuPerformance cpu, long[] inputIrqs) {
        long touches;
        synchronized (touchDowns) { touches = touchDownCount; }
        samples[nextSample] = new Sample(elapsedUs, retiredSteps, engineUs, executing,
                touches, guestPointerCount, guestKeyDownCount, cpu.hostSampleElapsedUs(),
                cpu.minReportedKhz(), cpu.maxReportedKhz(), cpu.thermalStatus(), cpu.hintThreadCount(), inputIrqs);
        nextSample = (nextSample + 1) % samples.length;
        sampleCount = Math.min(sampleCount + 1, samples.length);
    }

    String json() {
        StringBuilder out = new StringBuilder(1024);
        synchronized (touchDowns) {
            out.append("{\"session_start_elapsed_us\":").append(sessionStartUs)
                    .append(",\"android_touch_down_count\":").append(touchDownCount)
                    .append(",\"recent_android_touch_down_elapsed_us\":[");
            long first = Math.max(0, touchDownCount - touchDowns.length);
            for (long index = first; index < touchDownCount; ++index) {
                if (index != first) out.append(',');
                out.append(touchDowns[(int) (index % touchDowns.length)]);
            }
        }
        out.append("],\"guest_pointer_jni_count\":").append(guestPointerCount)
                .append(",\"guest_key_down_jni_count\":").append(guestKeyDownCount)
                .append(",\"samples\":[");
        int first = (nextSample + samples.length - sampleCount) % samples.length;
        Sample previous = null;
        for (int index = 0; index < sampleCount; ++index) {
            Sample sample = samples[(first + index) % samples.length];
            if (index > 0) out.append(',');
            writeSample(out, sample, previous);
            previous = sample;
        }
        return out.append("]}").toString();
    }

    private static void writeSample(StringBuilder out, Sample sample, Sample previous) {
        long steps = previous == null ? -1 : sample.retiredSteps - previous.retiredSteps;
        boolean rateAvailable = sample.executing && previous != null && previous.executing;
        out.append("{\"elapsed_us\":").append(sample.elapsedUs)
                .append(",\"retired_steps\":").append(sample.retiredSteps)
                .append(",\"engine_us\":").append(sample.engineUs)
                .append(",\"executing\":").append(sample.executing)
                .append(",\"steps_per_second\":");
        appendNumber(out, rateAvailable ? rate(steps, sample.elapsedUs - previous.elapsedUs) : -1);
        out.append(",\"engine_steps_per_second\":");
        appendNumber(out, rateAvailable ? rate(steps, sample.engineUs - previous.engineUs) : -1);
        out.append(",\"android_touch_down_count\":").append(sample.touchDownCount)
                .append(",\"guest_pointer_jni_count\":").append(sample.guestPointerCount)
                .append(",\"guest_key_down_jni_count\":").append(sample.guestKeyDownCount)
                .append(",\"host_freq_sample_elapsed_us\":");
        appendNumber(out, sample.hostSampleUs);
        out.append(",\"host_min_reported_khz\":");
        appendNumber(out, sample.hostMinKhz);
        out.append(",\"host_max_reported_khz\":");
        appendNumber(out, sample.hostMaxKhz);
        out.append(",\"android_thermal_status\":");
        appendNumber(out, sample.thermalStatus);
        out.append(",\"hint_threads\":").append(sample.hintThreads);
        long[] irqs = sample.inputIrqs;
        if (irqs != null && irqs.length == 4) {
            out.append(",\"irq1_assertions\":").append(irqs[0])
                    .append(",\"irq1_delivered_total\":").append(irqs[1])
                    .append(",\"irq12_assertions\":").append(irqs[2])
                    .append(",\"irq12_delivered_total\":").append(irqs[3]);
        }
        out.append('}');
    }

    private static long rate(long steps, long elapsedUs) {
        return steps < 0 || elapsedUs <= 0 ? -1 : Math.round(steps * 1_000_000.0 / elapsedUs);
    }

    private static void appendNumber(StringBuilder out, long value) {
        if (value < 0) out.append("null");
        else out.append(value);
    }
}
