package com.reversewindows.android;

final class VmStepRate {
    private long previousSteps = -1;
    private long previousExecutionUs = -1;

    void reset() {
        previousSteps = -1;
        previousExecutionUs = -1;
    }

    long sample(boolean executing, long steps, long executionUs) {
        if (!executing || steps < 0 || executionUs < 0) {
            reset();
            return -1;
        }
        long rate = previousSteps >= 0 && steps >= previousSteps && executionUs > previousExecutionUs
                ? Math.round((steps - previousSteps) * 1_000_000.0 / (executionUs - previousExecutionUs))
                : -1;
        previousSteps = steps;
        previousExecutionUs = executionUs;
        return rate;
    }
}
