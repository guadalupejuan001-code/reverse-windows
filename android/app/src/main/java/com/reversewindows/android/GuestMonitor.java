package com.reversewindows.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.util.Arrays;
import java.util.BitSet;

final class GuestMonitor extends View {
    interface Input {
        boolean onKey(KeyEvent event);
        void onText(CharSequence text);
        void onPresented();
    }

    private static final int[] VGA = {
        0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
        0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
        0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
        0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff,
    };
    private static final String LOW_GLYPHS = " ☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼";
    private static final String HIGH_GLYPHS =
            "ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»" +
            "░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀" +
            "αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ ";

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF frame = new RectF();
    private final Input input;
    private final VbePresentationStats vbeStats;
    private final byte[] cells = new byte[4000];
    private final char[] row = new char[80];
    private static final int BACKGROUND = 0xff080b10;
    private boolean hasText, opaqueVideo;
    // Each strip keeps its own Bitmap generation. Canvas can reuse the
    // textures of strips which received no pixels in this guest update.
    private Bitmap[] videoStripes;
    private int videoWidth;
    private int videoHeight;
    private int videoStripeHeight;
    private final RectF stripeFrame = new RectF();
    private final BitSet changedStripes = new BitSet();
    private VbeRenderNodes videoNodes;

    GuestMonitor(Context context, Input input, VbePresentationStats vbeStats) {
        super(context);
        this.input = input;
        this.vbeStats = vbeStats;
        setId(R.id.vm_monitor);
        setContentDescription("Pantalla del invitado");
        setFocusableInTouchMode(true);
    }

    private boolean commitPending,drawPending;
    private int generation;
    String dimensions(){return hasText?"80 × 25 celdas":videoStripes==null?"Pendiente":videoWidth+" × "+videoHeight+" px";}
    void show(VmWorker.Frame update) {
        if(isShown()){
            if(android.os.Build.VERSION.SDK_INT>=29 && isHardwareAccelerated()){
                if(!commitPending){
                    commitPending=true;int epoch=generation;
                    getViewTreeObserver().registerFrameCommitCallback(()->{
                        if(epoch!=generation)return;commitPending=false;
                        if(isShown())input.onPresented();
                    });
                }
            }else drawPending=true;
        }
        if (update.text != null) {
            System.arraycopy(update.text, 0, cells, 0, cells.length);
            hasText = true;
            discardVideo();
        } else {
            int[] video = update.video; // width, height, stripe height, count, then rectangles
            if (videoStripes == null || videoWidth != update.width || videoHeight != update.height ||
                    videoStripeHeight != video[2]) {
                discardVideo();
                videoWidth = update.width;
                videoHeight = update.height;
                videoStripeHeight = video[2];
                videoStripes = new Bitmap[(videoHeight + videoStripeHeight - 1) / videoStripeHeight];
            }
            int offset = 4;
            changedStripes.clear();
            for (int i = 0; i < video[3]; ++i) {
                int x = video[offset++];
                int y = video[offset++];
                int width = video[offset++];
                int height = video[offset++];
                // Native VBE scanout exports BGRX as opaque ARGB. A full rectangle
                // establishes coverage even when earlier packets arrived out of order.
                if (x == 0 && y == 0 && width == videoWidth && height == videoHeight) opaqueVideo = true;
                int end = y + height;
                while (y < end) {
                    int stripe = y / videoStripeHeight;
                    int top = stripe * videoStripeHeight;
                    int stripeHeight = Math.min(videoStripeHeight, videoHeight - top);
                    int rows = Math.min(end - y, top + stripeHeight - y);
                    if (videoStripes[stripe] == null) {
                        videoStripes[stripe] = Bitmap.createBitmap(
                                videoWidth, stripeHeight, Bitmap.Config.ARGB_8888);
                    }
                    videoStripes[stripe].setPixels(video, offset, width, x, y - top, width, rows);
                    changedStripes.set(stripe);
                    offset += width * rows;
                    y += rows;
                }
            }
            if (isShown() && isHardwareAccelerated()) {
                for (int i = changedStripes.nextSetBit(0); i >= 0; i = changedStripes.nextSetBit(i + 1)) {
                    videoStripes[i].prepareToDraw();
                }
            }
            hasText = false;
        }
        invalidate();
    }

    void reset() {
        generation++;commitPending=drawPending=false;
        vbeStats.reset();
        hasText = false;
        discardVideo();
        Arrays.fill(cells, (byte) 0);
        invalidate();
    }

    private void discardVideo() {
        videoNodes = null;
        videoStripes = null;
        opaqueVideo = false;
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        videoNodes = null;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if(drawPending){drawPending=false;input.onPresented();}
        if (videoStripes != null) fit(videoWidth, videoHeight);
        long skipped = 0;
        if (opaqueVideo) skipped = drawVideoBackground(canvas);
        else canvas.drawColor(BACKGROUND);
        if (hasText) {
            drawTextMode(canvas);
        } else if (videoStripes != null) {
            canvas.save();
            canvas.clipRect(frame);
            canvas.translate(frame.left, frame.top);
            canvas.scale(frame.width() / videoWidth, frame.height() / videoHeight);
            if (Build.VERSION.SDK_INT >= 29 && canvas.isHardwareAccelerated()) {
                if (videoNodes == null) videoNodes = new VbeRenderNodes(videoWidth, videoHeight, videoStripeHeight);
                int recorded = videoNodes.draw(canvas, videoStripes);
                vbeStats.drawn(VbePresentationStats.Backend.RENDER_NODES, recorded, skipped);
            } else {
                paint.setFilterBitmap(false);
                paint.setAntiAlias(false);
                for (int i = 0; i < videoStripes.length; ++i) {
                    Bitmap stripe = videoStripes[i];
                    if (stripe == null) continue;
                    int top = i * videoStripeHeight;
                    stripeFrame.set(0, top, videoWidth, top + stripe.getHeight());
                    canvas.drawBitmap(stripe, null, stripeFrame, paint);
                }
                paint.setAntiAlias(true);
                vbeStats.drawn(canvas.isHardwareAccelerated() ? VbePresentationStats.Backend.HARDWARE_CANVAS
                        : VbePresentationStats.Backend.SOFTWARE_CANVAS, 0, skipped);
            }
            canvas.restore();
        } else {
            paint.setColor(0xff81909f);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(getResources().getDisplayMetrics().scaledDensity * 14);
            paint.setTypeface(Typeface.DEFAULT);
            canvas.drawText("Pantalla del invitado", getWidth() / 2f,
                    getHeight() / 2f - (paint.ascent() + paint.descent()) / 2f, paint);
            paint.setTextAlign(Paint.Align.LEFT);
        }
    }

    private long drawVideoBackground(Canvas canvas) {
        // Keep the clear at fractional edges; fully covered interior pixels
        // will be painted by the opaque guest stripes in this drawing pass.
        int left = Math.max(0, (int)Math.ceil(frame.left));
        int top = Math.max(0, (int)Math.ceil(frame.top));
        int right = Math.min(getWidth(), (int)Math.floor(frame.right));
        int bottom = Math.min(getHeight(), (int)Math.floor(frame.bottom));
        if (right <= left || bottom <= top) {
            canvas.drawColor(BACKGROUND);
            return 0;
        }
        if (left != 0 || top != 0 || right != getWidth() || bottom != getHeight()) {
            int saved = canvas.save();
            if (canvas.clipOutRect(left, top, right, bottom)) canvas.drawColor(BACKGROUND);
            canvas.restoreToCount(saved);
        }
        return (long)(right - left) * (bottom - top);
    }

    private void fit(int width, int height) {
        float scale = Math.min((float) getWidth() / width, (float) getHeight() / height);
        float w = width * scale;
        float h = height * scale;
        frame.set((getWidth() - w) / 2, (getHeight() - h) / 2,
                (getWidth() + w) / 2, (getHeight() + h) / 2);
    }

    private void drawTextMode(Canvas canvas) {
        fit(640, 400);
        canvas.save();
        canvas.clipRect(frame);
        canvas.translate(frame.left, frame.top);
        canvas.scale(frame.width() / 640, frame.height() / 400);
        paint.setTypeface(Typeface.MONOSPACE);
        paint.setTextSize(13);
        for (int y = 0; y < 25; ++y) {
            for (int x = 0; x < 80; ) {
                int attribute = cells[(y * 80 + x) * 2 + 1] & 0xff;
                int end = x + 1;
                while (end < 80 && (cells[(y * 80 + end) * 2 + 1] & 0xff) == attribute) ++end;
                paint.setColor(VGA[(attribute >> 4) & 7]);
                canvas.drawRect(x * 8, y * 16, end * 8, (y + 1) * 16, paint);
                paint.setColor(VGA[attribute & 15]);
                for (int cell = x; cell < end; ++cell) {
                    row[cell] = glyph(cells[(y * 80 + cell) * 2] & 0xff);
                    if (row[cell] != ' ') canvas.drawText(row, cell, 1, cell * 8, y * 16 + 13, paint);
                }
                x = end;
            }
        }
        canvas.restore();
    }

    private static char glyph(int value) {
        if (value < 32) return LOW_GLYPHS.charAt(value);
        if (value < 127) return (char) value;
        if (value == 127) return '⌂';
        return HIGH_GLYPHS.charAt(value - 128);
    }

    void showKeyboard() {
        requestFocus();
        InputMethodManager keyboard = getContext().getSystemService(InputMethodManager.class);
        keyboard.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo attributes) {
        attributes.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        attributes.imeOptions = EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING;
        attributes.initialSelStart = 0;
        attributes.initialSelEnd = 0;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                if (text != null) input.onText(text);
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                return input.onKey(event);
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                if (beforeLength > 0) input.onText("\b");
                return true;
            }

            @Override
            public boolean performEditorAction(int actionCode) {
                input.onText("\n");
                return true;
            }
        };
    }
}
