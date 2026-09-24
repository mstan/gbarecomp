package org.gbarecomp;

/**
 * JNI entry points exported by the gbarecomp runtime (libmain.so).
 *
 * The native side lives in src/runtime/host_window.cpp. Every method here is
 * safe to call from the UI thread; the runtime reads the values on its own
 * thread at the next present.
 */
public final class GbaNative {
    private GbaNative() {}

    /**
     * Safe-area insets in physical pixels: display cutouts and mandatory
     * system-gesture regions. Host chrome (touch pad, native buttons) is laid
     * out inside them; the game image may extend underneath.
     */
    public static native void setSafeInsets(int left, int top, int right, int bottom);
}
