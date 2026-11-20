package com.reversewindows.android;

import android.view.KeyEvent;

final class GuestKeys {
    private static final int[] LETTER_SCANS = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };

    private GuestKeys() {}

    static int ascii(KeyEvent event) {
        return switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 13;
            case KeyEvent.KEYCODE_ESCAPE -> 27;
            case KeyEvent.KEYCODE_DEL -> 8;
            case KeyEvent.KEYCODE_TAB -> 9;
            default -> {
                int character = event.getUnicodeChar();
                yield character >= 32 && character <= 126 ? character : 0;
            }
        };
    }

    static int scan(int key, int ascii) {
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) {
            return LETTER_SCANS[key - KeyEvent.KEYCODE_A];
        }
        if (key >= KeyEvent.KEYCODE_1 && key <= KeyEvent.KEYCODE_9) {
            return key - KeyEvent.KEYCODE_1 + 2;
        }
        if (key >= KeyEvent.KEYCODE_F1 && key <= KeyEvent.KEYCODE_F10) {
            return key - KeyEvent.KEYCODE_F1 + 0x3b;
        }
        return switch (key) {
            case KeyEvent.KEYCODE_F11 -> 0x57;
            case KeyEvent.KEYCODE_NUMPAD_ENTER -> 0xe01c;
            case KeyEvent.KEYCODE_SHIFT_LEFT -> 0x2a;
            case KeyEvent.KEYCODE_SHIFT_RIGHT -> 0x36;
            case KeyEvent.KEYCODE_CTRL_LEFT -> 0x1d;
            case KeyEvent.KEYCODE_CTRL_RIGHT -> 0xe01d;
            case KeyEvent.KEYCODE_ALT_LEFT -> 0x38;
            case KeyEvent.KEYCODE_ALT_RIGHT -> 0xe038;
            case KeyEvent.KEYCODE_F12 -> 0x58;
            case KeyEvent.KEYCODE_DPAD_UP -> 0xe048;
            case KeyEvent.KEYCODE_DPAD_DOWN -> 0xe050;
            case KeyEvent.KEYCODE_DPAD_LEFT -> 0xe04b;
            case KeyEvent.KEYCODE_DPAD_RIGHT -> 0xe04d;
            case KeyEvent.KEYCODE_MOVE_HOME -> 0xe047;
            case KeyEvent.KEYCODE_MOVE_END -> 0xe04f;
            case KeyEvent.KEYCODE_PAGE_UP -> 0xe049;
            case KeyEvent.KEYCODE_PAGE_DOWN -> 0xe051;
            case KeyEvent.KEYCODE_INSERT -> 0xe052;
            case KeyEvent.KEYCODE_FORWARD_DEL -> 0xe053;
            default -> scanAscii(ascii);
        };
    }

    static boolean needsShift(char character) {
        return (character >= 'A' && character <= 'Z') || character == 34 || "~!@#$%^&*()_+{}|:<>?".indexOf(character) >= 0;
    }

    static int scanAscii(int character) {
        int lower = Character.toLowerCase(character);
        if (lower >= 'a' && lower <= 'z') return LETTER_SCANS[lower - 'a'];
        if (character >= '1' && character <= '9') return character - '1' + 2;
        return switch (character) {
            case '0', ')' -> 0x0b;
            case '!', '@', '#', '$', '%', '^', '&', '*', '(' -> "!@#$%^&*(".indexOf(character) + 2;
            case '\n', '\r' -> 0x1c;
            case '\t' -> 0x0f;
            case '\b' -> 0x0e;
            case 27 -> 0x01;
            case ' ' -> 0x39;
            case '-', '_' -> 0x0c;
            case '=', '+' -> 0x0d;
            case '[', '{' -> 0x1a;
            case ']', '}' -> 0x1b;
            case ';', ':' -> 0x27;
            case '\'', '"' -> 0x28;
            case '`', '~' -> 0x29;
            case '\\', '|' -> 0x2b;
            case ',', '<' -> 0x33;
            case '.', '>' -> 0x34;
            case '/', '?' -> 0x35;
            default -> 0;
        };
    }
}
