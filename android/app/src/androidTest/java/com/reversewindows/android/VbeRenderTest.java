package com.reversewindows.android;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.KeyEvent;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.util.Arrays;

/** Checks the displayed pixels after a full packet and a one-stripe update on the Android test device. */
public final class VbeRenderTest extends Instrumentation {
    private static final int WIDTH = 48, HEIGHT = 67, STRIPE = 32;
    private final VbePresentationStats stats = new VbePresentationStats();
    private GuestMonitor monitor;

    @Override public void onCreate(Bundle args) {
        super.onCreate(args);
        start();
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            Activity activity = startActivitySync(new Intent(getTargetContext(), MainActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            runOnMainSync(() -> {
                monitor = new GuestMonitor(activity, new GuestMonitor.Input() {
                    public boolean onKey(KeyEvent key) { return false; }
                    public void onText(CharSequence text) {}
                    public void onPresented() {}
                }, stats);
                activity.setContentView(monitor, new FrameLayout.LayoutParams(WIDTH, HEIGHT, Gravity.CENTER));
                monitor.show(packet(0, 0, WIDTH, HEIGHT, Color.RED));
            });
            awaitPicture(false);
            long first = new JSONObject(stats.json()).getLong("stripe_recordings");
            if (first != 3) throw new AssertionError("Expected three initial stripe recordings: " + stats.json());

            runOnMainSync(() -> monitor.show(packet(4, STRIPE + 1, 8, 5, Color.BLUE)));
            awaitPicture(true);
            JSONObject after = new JSONObject(stats.json());
            if (!"render_nodes".equals(after.getString("last_backend")) ||
                    after.getLong("stripe_recordings") != first + 1)
                throw new AssertionError("Expected one updated hardware stripe: " + after);

            Bitmap software = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
            runOnMainSync(() -> monitor.draw(new Canvas(software)));
            if (!matches(software, 0, 0, true)) throw new AssertionError("Software canvas pixels differ");
            long skipped = new JSONObject(stats.json()).getLong("background_pixels_skipped");
            if (skipped < (long)WIDTH * HEIGHT * 2) throw new AssertionError("Missing VBE clear savings");

            // A partial first packet still needs its background. An odd outer width
            // adds fractional letterbox edges to the next complete frame.
            Bitmap letterbox = Bitmap.createBitmap(WIDTH + 13, HEIGHT, Bitmap.Config.ARGB_8888);
            runOnMainSync(() -> {
                monitor.reset();
                monitor.layout(0, 0, WIDTH + 13, HEIGHT);
                monitor.show(packet(4, 4, 8, 5, Color.BLUE));
                monitor.draw(new Canvas(letterbox));
                if (letterbox.getPixel(1, 10) != 0xff080b10 || letterbox.getPixel(25, 20) != 0xff080b10 ||
                        letterbox.getPixel(13, 6) != Color.BLUE) throw new AssertionError("Partial-frame clear");
                monitor.show(packet(0, 0, WIDTH, HEIGHT, Color.RED));
                letterbox.eraseColor(Color.MAGENTA);
                monitor.draw(new Canvas(letterbox));
                for (int y = 0; y < HEIGHT; y++) {
                    for (int x = 0; x < WIDTH + 13; x++) {
                        if (x == 6 || x == WIDTH + 6) continue; // Android rounds half-pixel edges.
                        int expected = x < 6 || x > WIDTH + 6 ? 0xff080b10 : Color.RED;
                        if (letterbox.getPixel(x, y) != expected) throw new AssertionError("Letterbox pixel");
                    }
                }
            });
            letterbox.recycle();
            software.recycle();
            result.putString("stream", "VBE full and partial pixels match on HWUI and software Canvas.\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("stream", android.util.Log.getStackTraceString(error));
            finish(Activity.RESULT_CANCELED, result);
        }
    }

    private static VmWorker.Frame packet(int x, int y, int width, int height, int color) {
        int[] words = Arrays.copyOf(new int[]{WIDTH, HEIGHT, STRIPE, 1, x, y, width, height}, 8 + width * height);
        Arrays.fill(words, 8, words.length, color);
        return new VmWorker.Frame(words, null);
    }

    private void awaitPicture(boolean updated) {
        long deadline = SystemClock.uptimeMillis() + 5000;
        int[] origin = new int[2];
        do {
            waitForIdleSync();
            runOnMainSync(() -> monitor.getLocationOnScreen(origin));
            Bitmap screen = getUiAutomation().takeScreenshot();
            if (screen != null) {
                if (screen.getConfig() == Bitmap.Config.HARDWARE) {
                    Bitmap readable = screen.copy(Bitmap.Config.ARGB_8888, false);
                    screen.recycle();
                    screen = readable;
                }
                boolean match = matches(screen, origin[0], origin[1], updated);
                screen.recycle();
                if (match) return;
            }
            SystemClock.sleep(50);
        } while (SystemClock.uptimeMillis() < deadline);
        throw new AssertionError("Displayed VBE pixels differ: " + stats.json());
    }

    private static boolean matches(Bitmap frame, int left, int top, boolean updated) {
        if (left < 0 || top < 0 || left + WIDTH > frame.getWidth() || top + HEIGHT > frame.getHeight()) return false;
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                boolean patch = updated && x >= 4 && x < 12 && y > STRIPE && y <= STRIPE + 5;
                if (frame.getPixel(left + x, top + y) != (patch ? Color.BLUE : Color.RED)) return false;
            }
        }
        return true;
    }
}
