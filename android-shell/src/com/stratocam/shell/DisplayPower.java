package com.stratocam.shell;

import android.os.Build;
import android.os.IBinder;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;

/** Runs only as the authorized ADB shell, never with application permissions. */
public final class DisplayPower {
    public static void main(String[] args) throws Exception {
        Class<?> control = Class.forName("android.view.SurfaceControl");
        final IBinder token = (IBinder) (Build.VERSION.SDK_INT >= 29
                ? control.getMethod("getInternalDisplayToken").invoke(null)
                : control.getMethod("getBuiltInDisplay", int.class).invoke(null, 0));
        if (token == null) throw new IllegalStateException("No internal display available");
        final Method setPower = control.getMethod("setDisplayPowerMode", IBinder.class, int.class);
        final AtomicBoolean restored = new AtomicBoolean(false);
        Runnable restore = () -> {
            if (restored.compareAndSet(false, true)) {
                try { setPower.invoke(null, token, 2); }
                catch (Exception error) { System.err.println("Display restore failed: " + error); }
            }
        };
        Runtime.getRuntime().addShutdownHook(new Thread(restore, "RestoreDisplay"));
        try {
            setPower.invoke(null, token, 0);
            System.out.println("STRATOCAM_DISPLAY_OFF");
            System.out.flush();
            // The PC owns stdin. Stop, normal exit, or a lost ADB connection
            // closes the lease and restores the panel without putting Android to sleep.
            while (System.in.read() != -1) { }
        } finally {
            restore.run();
        }
    }
}
