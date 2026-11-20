package com.reversewindows.android;

import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.TextView;

/** The guest keeps the available canvas as touch controls open and close. */
final class ConsoleDeck extends LinearLayout {
    interface Input extends RgbKeyboard.Output { void pointer(int dx, int dy, int buttons); }

    final RgbKeyboard keyboard;
    private final MainActivity activity;
    private final Input input;
    private final GuestMonitor screen;
    private final FrameLayout idle;
    private final TextView caption;
    private final Pad pad;
    private final Button keyboardToggle, pointerToggle, faultButton;
    private final Button leftClick, rightClick;
    private int mask;
    private float sensitivity;

    ConsoleDeck(MainActivity activity, GuestMonitor screen, Input input, RgbKeyboard.Mode mode,
                int light, float sensitivity, Runnable settings, Runnable metrics, Runnable log) {
        super(activity);
        this.activity = activity;
        this.screen = screen;
        this.input = input;
        this.sensitivity = sensitivity;
        setId(R.id.vm_console);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setOrientation(HORIZONTAL);
        setMotionEventSplittingEnabled(true);
        setBackgroundColor(0xff080b10);

        LinearLayout stage = new LinearLayout(activity);
        stage.setOrientation(VERTICAL);
        stage.setMotionEventSplittingEnabled(true);
        addView(stage, new LayoutParams(0, -1, 1));

        FrameLayout display = new FrameLayout(activity);
        display.setId(R.id.vm_display);
        display.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        stage.addView(display, new LayoutParams(-1, 0, 1));
        display.addView(screen, new FrameLayout.LayoutParams(-1, -1));

        idle = new FrameLayout(activity);
        display.addView(idle, new FrameLayout.LayoutParams(-1, -1));
        ImageView art = new ImageView(activity);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        art.setImageResource(R.drawable.console_idle);
        art.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        idle.addView(art, new FrameLayout.LayoutParams(-1, -1));
        caption = activity.label("Máquina apagada", 12, 0xffe2f6f0);
        caption.setBackground(activity.panel(0xbb0c2024, 5, 0x55398d81));
        caption.setPadding(activity.dp(9), activity.dp(4), activity.dp(9), activity.dp(4));
        FrameLayout.LayoutParams captionBounds = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.START);
        captionBounds.setMarginStart(activity.dp(8));
        captionBounds.bottomMargin = activity.dp(7);
        idle.addView(caption, captionBounds);

        pad = new Pad(activity);
        display.addView(pad, new FrameLayout.LayoutParams(-1, -1));
        keyboard = new RgbKeyboard(activity, input, mode, light);
        keyboard.setId(R.id.vm_keyboard);
        keyboard.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        stage.addView(keyboard, new LayoutParams(-1, 0));

        ScrollView railScroll = new ScrollView(activity);
        railScroll.setFillViewport(true);
        railScroll.setVerticalScrollBarEnabled(false);
        railScroll.setBackgroundColor(0xff111a22);
        addView(railScroll, new LayoutParams(activity.dp(60), -1));
        LinearLayout rail = new LinearLayout(activity);
        rail.setOrientation(VERTICAL);
        rail.setGravity(Gravity.CENTER);
        rail.setPadding(activity.dp(3), 0, activity.dp(3), 0);
        rail.setMotionEventSplittingEnabled(true);
        railScroll.addView(rail);

        Button menu = railButton(rail, "Menú", R.id.vm_console_menu);
        menu.setContentDescription("Menú de la consola");
        menu.setOnClickListener(v -> {
            release();
            PopupMenu popup = new PopupMenu(activity, menu);
            popup.getMenu().add("Configuración").setOnMenuItemClickListener(item -> { settings.run(); return true; });
            popup.getMenu().add("Métricas").setOnMenuItemClickListener(item -> { metrics.run(); return true; });
            popup.getMenu().add("Registro").setOnMenuItemClickListener(item -> { log.run(); return true; });
            popup.show();
        });
        keyboardToggle = railButton(rail, "Teclado", R.id.vm_toggle_keyboard);
        keyboardToggle.setOnClickListener(v -> setKeyboardVisible(keyboard.getVisibility() != VISIBLE));
        pointerToggle = railButton(rail, "Ratón", R.id.vm_toggle_pointer);
        pointerToggle.setOnClickListener(v -> setPointerVisible(pad.getVisibility() != VISIBLE));
        faultButton = railButton(rail, "Ver fallo", R.id.vm_console_fault);
        faultButton.setTextColor(MainActivity.ERROR);
        faultButton.setAccessibilityLiveRegion(ACCESSIBILITY_LIVE_REGION_POLITE);
        faultButton.setOnClickListener(v -> log.run());
        faultButton.setVisibility(GONE);
        leftClick = mouseButton(rail, "Izq.", "Clic izquierdo", R.id.vm_left_click, 1);
        rightClick = mouseButton(rail, "Der.", "Clic derecho", R.id.vm_right_click, 2);
        setKeyboardVisible(false);
        setPointerVisible(false);
    }

    private Button railButton(LinearLayout rail, String label, int id) {
        Button button = activity.action(label, false, () -> {});
        button.setId(id);
        button.setTextSize(11);
        button.setPadding(0, 0, 0, 0);
        LayoutParams bounds = new LayoutParams(-1, activity.dp(48));
        bounds.topMargin = bounds.bottomMargin = activity.dp(2);
        rail.addView(button, bounds);
        return button;
    }

    private Button mouseButton(LinearLayout rail, String label, String description, int id, int bit) {
        Button button = railButton(rail, label, id);
        button.setContentDescription(description);
        button.setOnClickListener(v -> { setButton(bit, true); setButton(bit, false); });
        button.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN -> { button.setPressed(true); setButton(bit, true); }
                case MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { button.setPressed(false); setButton(bit, false); }
            }
            return true;
        });
        return button;
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int available = MeasureSpec.getSize(heightSpec);
        keyboard.getLayoutParams().height = Math.min(available / 3, activity.dp(192));
        super.onMeasure(widthSpec, heightSpec);
    }

    void sensitivity(float value) { sensitivity = value; }

    void state(boolean live, boolean fault, CharSequence description) {
        screen.setVisibility(live ? VISIBLE : GONE);
        idle.setVisibility(live ? GONE : VISIBLE);
        if (!live) caption.setText(description);
        faultButton.setVisibility(fault ? VISIBLE : GONE);
    }

    private void setKeyboardVisible(boolean visible) {
        if (visible) {
            InputMethodManager ime = activity.getSystemService(InputMethodManager.class);
            if (ime != null) ime.hideSoftInputFromWindow(screen.getWindowToken(), 0);
        } else {
            keyboard.release();
        }
        keyboard.setVisibility(visible ? VISIBLE : GONE);
        keyboardToggle.setSelected(visible);
        keyboardToggle.setContentDescription(visible ? "Ocultar teclado" : "Mostrar teclado");
    }

    private void setPointerVisible(boolean visible) {
        if (!visible) releasePointer();
        int visibility = visible ? VISIBLE : GONE;
        pad.setVisibility(visibility);
        leftClick.setVisibility(visibility);
        rightClick.setVisibility(visibility);
        pointerToggle.setSelected(visible);
        pointerToggle.setContentDescription(visible ? "Desactivar ratón" : "Activar ratón");
    }

    boolean dismissInputs() {
        boolean open = keyboard.getVisibility() == VISIBLE || pad.getVisibility() == VISIBLE;
        if (open) {
            setKeyboardVisible(false);
            setPointerVisible(false);
        }
        return open;
    }

    private void setButton(int bit, boolean pressed) {
        pad.flush();
        if (pressed) pad.tapCandidate = false;
        int next = pressed ? mask | bit : mask & ~bit;
        if (next != mask) {
            mask = next;
            input.pointer(0, 0, mask);
        }
    }

    private void releasePointer() {
        pad.finishGesture();
        if (mask != 0) {
            mask = 0;
            input.pointer(0, 0, 0);
        }
        leftClick.setPressed(false);
        rightClick.setPressed(false);
    }

    void release() {
        keyboard.release();
        releasePointer();
    }

    private final class Pad extends View {
        private final int slop;
        private final Runnable sendMotion = this::flush;
        private int finger = -1;
        private float startX, startY, lastX, lastY, dx, dy;
        private boolean tapCandidate;

        Pad(MainActivity activity) {
            super(activity);
            slop = ViewConfiguration.get(activity).getScaledTouchSlop();
            setId(R.id.vm_touchpad);
            setContentDescription("Panel táctil del invitado");
            setClickable(true);
        }

        void flush() {
            removeCallbacks(sendMotion);
            int x = (int)dx, y = (int)dy;
            dx -= x;
            dy -= y;
            if (x != 0 || y != 0) input.pointer(x, -y, mask);
        }

        void finishGesture() {
            flush();
            finger = -1;
            tapCandidate = false;
            dx = dy = 0;
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                finger = event.getPointerId(0);
                startX = lastX = event.getX();
                startY = lastY = event.getY();
                tapCandidate = mask == 0;
                return true;
            }
            if (action == MotionEvent.ACTION_CANCEL) { finishGesture(); return true; }
            if (action == MotionEvent.ACTION_POINTER_DOWN) tapCandidate = false;
            int index = event.findPointerIndex(finger);
            if (index < 0) return true;
            if (action == MotionEvent.ACTION_POINTER_UP && event.getActionIndex() != index) return true;
            if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                float x = event.getX(index), y = event.getY(index);
                if (Math.abs(x - startX) > slop || Math.abs(y - startY) > slop) tapCandidate = false;
                float gain = sensitivity / getResources().getDisplayMetrics().density;
                dx += (x - lastX) * gain;
                dy += (y - lastY) * gain;
                lastX = x;
                lastY = y;
                if (action == MotionEvent.ACTION_MOVE) {
                    removeCallbacks(sendMotion);
                    postOnAnimation(sendMotion);
                } else {
                    boolean click = tapCandidate && action == MotionEvent.ACTION_UP;
                    finishGesture();
                    if (click) performClick();
                }
            }
            return true;
        }

        @Override public boolean performClick() {
            super.performClick();
            if (mask == 0) { setButton(1, true); setButton(1, false); }
            return true;
        }
    }
}
