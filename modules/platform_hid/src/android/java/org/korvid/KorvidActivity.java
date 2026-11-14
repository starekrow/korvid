// Korvid on Android: the one class of Java in a game's package, and only for what Android
// gives to Java alone. A NativeActivity hands native code raw key events; text as a person
// types it (a layout's characters, an input method's composed words, the on-screen
// keyboard) arrives through an InputConnection, which only a View can offer. This activity
// adds a view that offers one and passes what is committed to the engine as UTF-8
// (platform_hid/src/android/host_window_android.cc registers the native methods).
//
// kormake compiles it with javac and d8 into the package's classes.dex (package_apk.lua)
// when a JDK is installed; without one the package uses NativeActivity itself and has no
// composed text.
package org.korvid;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import java.nio.charset.StandardCharsets;

public class KorvidActivity extends NativeActivity {
    // Typed text, UTF-8, and a count of characters to delete before the cursor. Called on
    // the UI thread; the engine takes them at its next pump.
    private static native void nativeText(byte[] utf8);
    private static native void nativeBackspace(int count);

    private TextSink sink;
    private boolean textOn;

    // The view the input method talks to. It draws nothing and is one pixel; it holds the
    // focus only while a text field wants typing.
    private final class TextSink extends View {
        TextSink(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override public boolean onCheckIsTextEditor() { return textOn; }

        @Override public InputConnection onCreateInputConnection(EditorInfo info) {
            if (!textOn) return null;
            // Plain text, committed as typed: no suggestions strip and no autocorrect, which
            // would hold words back as composing text and rewrite them after the fact.
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                    | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
            info.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_DONE;
            return new Connection(this);
        }
    }

    // What the input method commits goes to the engine; what it is still composing (an
    // input method for Chinese or Japanese, a swipe) is kept until it is finished.
    private final class Connection extends BaseInputConnection {
        private String composing = "";

        Connection(View view) { super(view, false); }

        @Override public boolean commitText(CharSequence text, int newCursorPosition) {
            composing = "";
            send(text.toString());
            return true;
        }

        @Override public boolean setComposingText(CharSequence text, int newCursorPosition) {
            composing = text.toString();
            return true;
        }

        @Override public boolean finishComposingText() {
            if (!composing.isEmpty()) send(composing);
            composing = "";
            return true;
        }

        @Override public boolean deleteSurroundingText(int before, int after) {
            if (before > 0) nativeBackspace(before);
            return true;
        }

        @Override public boolean performEditorAction(int action) {
            // The keyboard's Done: Enter, as a key, through the window's own queue.
            sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER));
            sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER));
            return true;
        }
    }

    private static void send(String text) {
        if (!text.isEmpty()) nativeText(text.getBytes(StandardCharsets.UTF_8));
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        sink = new TextSink(this);
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
    }

    // From the engine (HostWindow::text_input), on its own thread: the keyboard up with the
    // sink focused, or down with the focus back on the game's surface.
    public void textInput(final boolean on) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                textOn = on;
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (on) {
                    sink.requestFocus();
                    imm.restartInput(sink);
                    imm.showSoftInput(sink, 0);
                } else {
                    imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
                    sink.clearFocus();
                    imm.restartInput(sink);
                }
            }
        });
    }

    // A hardware keyboard's keys reach native code first, as keys. While text is wanted the
    // engine leaves the printable ones unhandled, so they arrive here, where the layout's
    // character for the key (Shift, AltGr and dead keys applied) is known.
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (textOn && event.getAction() == KeyEvent.ACTION_DOWN) {
            int c = event.getUnicodeChar();
            if (c >= 32 && (c & KeyCharacterMap_COMBINING_ACCENT) == 0) {
                send(new String(Character.toChars(c)));
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    private static final int KeyCharacterMap_COMBINING_ACCENT = 0x80000000;
}
