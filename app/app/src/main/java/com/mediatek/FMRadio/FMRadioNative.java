package com.mediatek.FMRadio;

// MediaTek's FM library (/system/lib/libfmjni.so) registers its natives on this exact class name
// and checks every one: names and signatures below are read from the library's own table
// (docs/device/README.md, FM radio). Frequencies are MHz as float (101.7f); autoscan answers tenths
// of MHz (1017). Call from one thread at a time (player/FmRadio.kt).
public final class FMRadioNative {
    static {
        System.loadLibrary("fmjni");
    }

    private FMRadioNative() {}

    public static native boolean opendev();
    public static native boolean closedev();
    public static native boolean powerup(float frequency);
    public static native boolean powerdown(int type);
    public static native boolean tune(float frequency);
    public static native float seek(float frequency, boolean up);
    public static native short[] autoscan();
    public static native boolean stopscan();
    public static native int readRssi();
    public static native int rdsset(boolean on);
    public static native short readrds();
    public static native short getPI();
    public static native byte getPTY();
    public static native byte[] getPS();
    public static native byte[] getLRText();
    public static native short activeAF();
    public static native short[] getAFList();
    public static native short activeTA();
    public static native short deactiveTA();
    public static native int setmute(boolean mute);
    public static native int getchipid();
    public static native int isRDSsupport();
    public static native int isFMPoweredUp();
    public static native boolean setFMViaBTController(boolean on);
    public static native int switchAntenna(int antenna);
    public static native boolean stereoMono();
    public static native short readCapArray();
    public static native short readRdsBler();
    public static native boolean setStereoMono(boolean mono);
    public static native int[] getHardwareVersion();
    public static native short[] scannew(int a, int b, int c);
    public static native int seeknew(int a, int b, int c, int d, int e, int f);
    public static native boolean tunenew(int a, int b, int c, int d);
}
