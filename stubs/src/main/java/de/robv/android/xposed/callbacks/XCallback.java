package de.robv.android.xposed.callbacks;

public abstract class XCallback implements Comparable<XCallback> {

    public static final int PRIORITY_DEFAULT = 50;

    public final int priority;

    public XCallback() {
        this.priority = PRIORITY_DEFAULT;
    }

    public XCallback(int priority) {
        this.priority = priority;
    }

    @Override
    public int compareTo(XCallback other) {
        return Integer.compare(this.priority, other.priority);
    }

    public static class Param {
        public Object[] callbacks;
    }
}
