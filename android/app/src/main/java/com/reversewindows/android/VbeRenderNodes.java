package com.reversewindows.android;

import android.annotation.TargetApi;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RecordingCanvas;
import android.graphics.Rect;
import android.graphics.RenderNode;

/** Retains one Android display list per stripe in the GPU-VIRTUAL framebuffer plan. */
@TargetApi(29)
final class VbeRenderNodes {
    private final RenderNode frame = new RenderNode("VBE");
    private final RenderNode[] stripes;
    private final int[] generations;
    private final Rect bounds = new Rect();
    private final Paint paint = new Paint(0);

    VbeRenderNodes(int width, int height, int stripeHeight) {
        frame.setPosition(0, 0, width, height);
        stripes = new RenderNode[(height + stripeHeight - 1) / stripeHeight];
        generations = new int[stripes.length];
        for (int i = 0; i < stripes.length; ++i) {
            int top = i * stripeHeight;
            stripes[i] = new RenderNode("VBE " + i);
            stripes[i].setPosition(0, top, width, Math.min(top + stripeHeight, height));
        }
        paint.setFilterBitmap(false);
    }

    int draw(Canvas target, Bitmap[] pixels) {
        int recorded = 0;
        for (int i = 0; i < stripes.length; ++i) {
            Bitmap bitmap = pixels[i];
            if (bitmap == null) continue;
            RenderNode stripe = stripes[i];
            int generation = bitmap.getGenerationId();
            if (generations[i] == generation && stripe.hasDisplayList()) continue;
            RecordingCanvas canvas = stripe.beginRecording();
            try {
                bounds.set(0, 0, bitmap.getWidth(), bitmap.getHeight());
                canvas.drawBitmap(bitmap, null, bounds, paint);
            } finally {
                stripe.endRecording();
            }
            generations[i] = generation;
            ++recorded;
        }
        if (!frame.hasDisplayList()) {
            RecordingCanvas canvas = frame.beginRecording();
            try {
                for (RenderNode stripe : stripes) canvas.drawRenderNode(stripe);
            } finally {
                frame.endRecording();
            }
        }
        target.drawRenderNode(frame);
        return recorded;
    }
}
