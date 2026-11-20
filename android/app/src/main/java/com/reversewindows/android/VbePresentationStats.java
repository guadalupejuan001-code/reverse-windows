package com.reversewindows.android;

/** The UI records VBE draw passes. The VM worker snapshots them in the session diagnostic. */
final class VbePresentationStats {
    enum Backend {
        RENDER_NODES("render_nodes"), HARDWARE_CANVAS("hardware_canvas"), SOFTWARE_CANVAS("software_canvas");

        final String id;
        Backend(String id) { this.id = id; }
    }

    private Backend last;
    private long acceleratedDraws;
    private long softwareDraws;
    private long recordedStripes, backgroundPixelsSkipped;

    synchronized void reset() {
        last = null;
        acceleratedDraws = softwareDraws = recordedStripes = backgroundPixelsSkipped = 0;
    }

    synchronized void drawn(Backend backend, int recordings, long skipped) {
        last = backend;
        if (backend == Backend.SOFTWARE_CANVAS) ++softwareDraws;
        else ++acceleratedDraws;
        recordedStripes += recordings;
        backgroundPixelsSkipped += skipped;
    }

    synchronized String json() {
        return "{\"last_backend\":" + (last == null ? "null" : "\"" + last.id + "\"")
                + ",\"accelerated_canvas_draws\":" + acceleratedDraws
                + ",\"software_canvas_draws\":" + softwareDraws
                + ",\"stripe_recordings\":" + recordedStripes
                + ",\"background_pixels_skipped\":" + backgroundPixelsSkipped + "}";
    }
}
