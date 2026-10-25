package org.midibottleneck.core.route;

/**
 * Runs a call that may hang (a binder call into the system's MIDI service, a device that never answers) on its own
 * daemon thread and waits for it at most a given time. If the call is still running when the time is up, the thread is
 * abandoned (it keeps running until the call returns, or forever) and the caller goes on. Nothing here ever blocks the
 * caller longer than {@code timeoutMs}. Plain Java (host-tested).
 */
public final class BoundedCall {
    private BoundedCall() {}

    /** The outcome of {@link #run}: finished or abandoned, and where the thread was stuck. */
    public static final class Result {
        public final boolean finished;
        /** Short text of where the abandoned thread is stuck (top frames of its stack), or "" when finished. */
        public final String where;
        public final Thread thread;

        Result(boolean finished, String where, Thread thread) {
            this.finished = finished;
            this.where = where;
            this.thread = thread;
        }
    }

    /**
     * Runs {@code task} on a new daemon thread named {@code name} and waits up to {@code timeoutMs}. {@code onAbandon}
     * (may be null) runs on the caller's thread at the moment it gives up, before {@code lateTail} can possibly run, and
     * must not block. {@code lateTail} (may be null) runs on the task's thread after the task, only when the caller had
     * already given up; it is how a late success is cleaned up (for example, closing a device nobody wants any more).
     */
    public static Result run(String name, long timeoutMs, final Runnable task, Runnable onAbandon, final Runnable lateTail) {
        final Object gate = new Object();
        final boolean[] state = new boolean[2];   // [0] finished, [1] abandoned
        Thread thread = new Thread(new Runnable() {
            @Override public void run() {
                try {
                    task.run();
                } catch (RuntimeException ignored) {
                }
                boolean late;
                synchronized (gate) {
                    state[0] = true;
                    late = state[1];
                    gate.notifyAll();
                }
                if (late && lateTail != null) {
                    try { lateTail.run(); } catch (RuntimeException ignored) { }
                }
            }
        }, name);
        thread.setDaemon(true);
        thread.start();
        long end = System.nanoTime() + timeoutMs * 1000000L;
        synchronized (gate) {
            while (!state[0]) {
                long left = end - System.nanoTime();
                if (left <= 0) break;
                try {
                    gate.wait(left / 1000000L, (int) (left % 1000000L));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            if (state[0]) return new Result(true, "", thread);
            state[1] = true;
            if (onAbandon != null) {
                try { onAbandon.run(); } catch (RuntimeException ignored) { }
            }
        }
        return new Result(false, describe(thread), thread);
    }

    /** The top frames of a thread's stack as one line ("a.B.m <- c.D.n"). */
    public static String describe(Thread thread) {
        StringBuilder sb = new StringBuilder();
        StackTraceElement[] stack;
        try {
            stack = thread.getStackTrace();
        } catch (RuntimeException e) {
            return "(stack not available)";
        }
        int n = Math.min(stack.length, 6);
        for (int i = 0; i < n; i++) {
            if (i > 0) sb.append(" <- ");
            sb.append(stack[i].getClassName()).append('.').append(stack[i].getMethodName());
            if (stack[i].isNativeMethod()) sb.append("(native)");
        }
        return sb.length() == 0 ? "(no stack)" : sb.toString();
    }

    /** Starts {@code task} on a detached daemon thread. */
    public static Thread detached(String name, Runnable task) {
        Thread t = new Thread(task, name);
        t.setDaemon(true);
        t.start();
        return t;
    }
}
