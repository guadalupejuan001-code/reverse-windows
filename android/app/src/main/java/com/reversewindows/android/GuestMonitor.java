package com.reversewindows.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.util.Arrays;

final class GuestMonitor extends View {
    interface Input {
        boolean onKey(KeyEvent event);
        void onText(CharSequence text);
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
    private final byte[] cells = new byte[4000];
    private final char[] row = new char[80];
    private boolean hasText;
    private Bitmap bitmap;

    GuestMonitor(Context context, Input input) {
        super(context);
        this.input = input;
        setId(R.id.vm_monitor);
        setContentDescription("Pantalla del invitado");
        setFocusableInTouchMode(true);
    }

    void show(VmWorker.Frame update) {
        if (update.text != null) {
            System.arraycopy(update.text, 0, cells, 0, cells.length);
            hasText = true;
            discardBitmap();
        } else {
            if (bitmap == null || bitmap.getWidth() != update.width || bitmap.getHeight() != update.height) {
                discardBitmap();
                bitmap = Bitmap.createBitmap(update.width, update.height, Bitmap.Config.ARGB_8888);
            }
            bitmap.setPixels(update.pixels, 0, update.width, 0, 0, update.width, update.height);
            hasText = false;
        }
        invalidate();
    }

    void reset() {
        hasText = false;
        discardBitmap();
        Arrays.fill(cells, (byte) 0);
        invalidate();
    }

    private void discardBitmap() {
        bitmap = null;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(0xff080b10);
        if (hasText) {
            drawTextMode(canvas);
        } else if (bitmap != null) {
            fit(bitmap.getWidth(), bitmap.getHeight());
            paint.setFilterBitmap(false);
            canvas.drawBitmap(bitmap, null, frame, paint);
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
