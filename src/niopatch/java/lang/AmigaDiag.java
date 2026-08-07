package java.lang;

/**
 * Diagnostics that need to see the process from the inside.
 *
 * Lives in java.lang and ships in niopatch.zip because its native half is in
 * libjava, next to the process code it exists to measure.  Nothing in the
 * runtime depends on it; it is here so that a test can answer a question that
 * cannot be answered from outside.
 *
 * PIPE: cannot be listed -- it is a read/write device with no directory -- so
 * "how many pipe handles are open" has no answer from the shell.  What CAN be
 * answered is how many descriptors this process itself holds, and that is the
 * discriminating half of the question: a leak of the parent's ends shows up
 * here, a leak of the ends handed to a child does not, because those belong to
 * the child once spawnvpe has duplicated them.
 */
public final class AmigaDiag {

    private AmigaDiag() {
    }

    /**
     * Open file descriptors held by this process, or -1 where it cannot be
     * determined.
     *
     * Counts rather than enumerates: the number is what a leak hunt needs, and
     * a delta across an operation says more than any single reading.
     */
    public static native int openFdCount();
}
