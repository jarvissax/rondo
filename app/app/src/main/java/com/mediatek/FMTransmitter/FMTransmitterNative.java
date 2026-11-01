package com.mediatek.FMTransmitter;

// Never called: libfmjni registers natives on this class too and fails to load without it. The
// signatures are the library's own (docs/device/README.md, FM radio).
public final class FMTransmitterNative {
    private FMTransmitterNative() {}

    public static native boolean opendev();
    public static native boolean closedev();
    public static native boolean powerup(float frequency);
    public static native boolean powerdown(int type);
    public static native boolean tune(float frequency);
    public static native float seek(float frequency, boolean up);
    public static native short[] autoscan();
    public static native boolean stopscan();
    public static native int rdsset(boolean on);
    public static native short readrds();
    public static native byte[] getPS();
    public static native byte[] getLRText();
    public static native short activeAF();
    public static native short activeTA();
    public static native short deactiveTA();
    public static native int setmute(boolean mute);
    public static native int isRDSsupport();
    public static native int isTXSupport();
    public static native boolean powerupTX(float frequency);
    public static native short[] getTXFreqList(float frequency, int a, int b);
    public static native int isRDSTXSupport();
    public static native boolean setRDSTXEnabled(boolean on);
    public static native boolean setRDSTX(short pi, char[] ps, short[] rds, int count);
    public static native boolean tuneTX(float frequency);
}
