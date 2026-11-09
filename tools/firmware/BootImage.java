// The Y1's boot image (Android's boot.img, MediaTek's 512-byte header on the ramdisk), edited for Rondo
// 2.0: init.rc starts Rondo, not the Java layer (firmware/build-native.sh).
//
//   java tools/firmware/BootImage.java in.img out.img
//
// In init.rc, zygote becomes disabled (no Java layer), and the "rondo" service runs /system/rondo/start.
// SurfaceFlinger still starts, to set the panel up as it always has, and the start script stops it
// before Rondo draws to the framebuffer itself. Everything else (the kernel, its command line, the other
// files of the ramdisk) stays byte for byte; the header's sizes and SHA-1 are made again.
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

public class BootImage {
  static final int MTK_MAGIC = 0x58881688;

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      System.err.println("usage: java BootImage.java in.img out.img");
      System.exit(2);
    }
    byte[] img = Files.readAllBytes(Paths.get(args[0]));
    ByteBuffer h = ByteBuffer.wrap(img).order(ByteOrder.LITTLE_ENDIAN);
    if (!new String(img, 0, 8, StandardCharsets.US_ASCII).equals("ANDROID!")) throw new IOException("not a boot image");
    int kernelSize = h.getInt(8), ramdiskSize = h.getInt(16), secondSize = h.getInt(24), page = h.getInt(36);
    int kernelAt = page, ramdiskAt = kernelAt + align(kernelSize, page), secondAt = ramdiskAt + align(ramdiskSize, page);
    byte[] kernel = Arrays.copyOfRange(img, kernelAt, kernelAt + kernelSize);
    byte[] ramdisk = Arrays.copyOfRange(img, ramdiskAt, ramdiskAt + ramdiskSize);
    byte[] second = Arrays.copyOfRange(img, secondAt, secondAt + secondSize);
    int tailAt = secondAt + align(secondSize, page);
    byte[] tail = tailAt < img.length ? Arrays.copyOfRange(img, tailAt, img.length) : new byte[0];

    // MediaTek's header: magic, the size of what follows, the name ("ROOTFS"), padding to 512
    byte[] mtk = null;
    byte[] gz = ramdisk;
    if (ramdisk.length > 512 && ByteBuffer.wrap(ramdisk).order(ByteOrder.LITTLE_ENDIAN).getInt(0) == MTK_MAGIC) {
      mtk = Arrays.copyOf(ramdisk, 512);
      gz = Arrays.copyOfRange(ramdisk, 512, ramdisk.length);
    }
    byte[] cpio = new GZIPInputStream(new ByteArrayInputStream(gz)).readAllBytes();
    List<Entry> entries = readCpio(cpio);
    Entry rc = entries.stream().filter(e -> e.name.equals("init.rc")).findFirst().orElseThrow(() -> new IOException("no init.rc"));
    String text = new String(rc.data, StandardCharsets.UTF_8);
    String edited = editInitRc(text);
    rc.data = edited.getBytes(StandardCharsets.UTF_8);
    rc.fields[6] = rc.data.length;  // c_filesize
    // the services a Y1 without Android has no use for, in the files a normal boot reads
    int off = 0;
    for (Entry e : entries) {
      if (!e.name.equals("init.rc") && !e.name.equals("init.modem.rc")) continue;
      String[] r = disableServices(new String(e.data, StandardCharsets.UTF_8));
      e.data = r[0].getBytes(StandardCharsets.UTF_8);
      e.fields[6] = e.data.length;
      off += Integer.parseInt(r[1]);
    }
    if (off != UNUSED.size()) throw new IllegalStateException("found " + off + " of the " + UNUSED.size() + " unused services");

    ByteArrayOutputStream z = new ByteArrayOutputStream();
    try (GZIPOutputStream out = new GZIPOutputStream(z) {{ def.setLevel(Deflater.BEST_COMPRESSION); }}) {
      out.write(writeCpio(entries));
    }
    byte[] newGz = z.toByteArray();
    byte[] newRamdisk = newGz;
    if (mtk != null) {
      newRamdisk = new byte[512 + newGz.length];
      System.arraycopy(mtk, 0, newRamdisk, 0, 512);
      ByteBuffer.wrap(newRamdisk).order(ByteOrder.LITTLE_ENDIAN).putInt(4, newGz.length);
      System.arraycopy(newGz, 0, newRamdisk, 512, newGz.length);
    }

    byte[] header = Arrays.copyOf(img, page);
    ByteBuffer nh = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
    nh.putInt(16, newRamdisk.length);
    // mkbootimg's id: SHA-1 of each part and its size
    MessageDigest sha = MessageDigest.getInstance("SHA-1");
    for (byte[] part : new byte[][] {kernel, newRamdisk, second}) {
      sha.update(part);
      sha.update(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(part.length).array());
    }
    byte[] id = sha.digest();
    Arrays.fill(header, 576, 576 + 32, (byte) 0);
    System.arraycopy(id, 0, header, 576, id.length);

    ByteArrayOutputStream o = new ByteArrayOutputStream();
    o.write(header);
    writePadded(o, kernel, page);
    writePadded(o, newRamdisk, page);
    if (second.length > 0) writePadded(o, second, page);
    o.write(tail);
    Files.write(Paths.get(args[1]), o.toByteArray());
    System.out.println("init.rc: zygote and " + UNUSED.size() + " unused services disabled, the rondo service added; ramdisk " + ramdiskSize + " -> "
        + newRamdisk.length + " bytes");
  }

  // Measured on the Y1 with the Java layer gone (Nov 9): Wi-Fi, Bluetooth, sound, FM, the card and
  // NVRAM work with all of these stopped, and 4.5 MB more is free. The Y1 has no modem, SIM, GPS or TV
  // tuner, and no apps to install, sign or play protected media for.
  static final java.util.List<String> UNUSED = java.util.Arrays.asList(
      "ccci_fsd", "ccci_mdinit", "gsm0710muxd", "ril-daemon", "mdlogger",  // the modem
      "agpsd", "matv", "DMAgent", "netdiag",                               // GPS, TV, device management, diagnostics
      "drm", "installd", "keystore");                                        // Android's apps

  // "disabled" under each unused service; returns the text and how many were found
  static String[] disableServices(String text) {
    StringBuilder out = new StringBuilder();
    String[] lines = text.split("\n", -1);
    int found = 0;
    for (int i = 0; i < lines.length; i++) {
      out.append(lines[i]);
      if (i < lines.length - 1) out.append('\n');
      String[] w = lines[i].trim().split("\\s+");
      if (w.length >= 2 && w[0].equals("service") && UNUSED.contains(w[1])) {
        out.append("    disabled  # Rondo 2.0: not needed on the Y1 without Android\n");
        found++;
      }
    }
    return new String[] {out.toString(), Integer.toString(found)};
  }

  static String editInitRc(String text) {
    StringBuilder out = new StringBuilder();
    String[] lines = text.split("\n", -1);
    int disabled = 0;
    for (int i = 0; i < lines.length; i++) {
      out.append(lines[i]);
      if (i < lines.length - 1) out.append('\n');
      String l = lines[i].trim();
      if (l.startsWith("service zygote ")) {
        out.append("    disabled  # Rondo 2.0: no Java layer\n");
        disabled++;
      }
    }
    if (disabled != 1) throw new IllegalStateException("zygote not found in init.rc");
    if (text.contains("service rondo ")) throw new IllegalStateException("init.rc already has Rondo");
    out.append("\n# Rondo 2.0: the player itself, drawn straight to the framebuffer (tools/firmware/BootImage.java).\n"
        + "# /system/rondo/start runs the newest update in /data/rondo or the firmware's own copy, and runs it\n"
        + "# again whenever it ends.\n"
        + "service rondo /system/bin/sh /system/rondo/start\n"
        + "    class main\n"
        + "    user root\n"
        + "    group root\n");
    return out.toString();
  }

  static int align(int n, int page) { return (n + page - 1) / page * page; }

  static void writePadded(OutputStream o, byte[] b, int page) throws IOException {
    o.write(b);
    o.write(new byte[align(b.length, page) - b.length]);
  }

  // cpio "newc": a 110-byte ASCII header (magic and 13 hex fields), the name, the data, each padded to 4
  static class Entry {
    long[] fields = new long[13];
    String name;
    byte[] data;
  }

  static List<Entry> readCpio(byte[] b) throws IOException {
    List<Entry> out = new ArrayList<>();
    int at = 0;
    while (at + 110 <= b.length) {
      String magic = new String(b, at, 6, StandardCharsets.US_ASCII);
      if (!magic.equals("070701") && !magic.equals("070702")) throw new IOException("cpio: bad magic at " + at);
      Entry e = new Entry();
      for (int k = 0; k < 13; k++) e.fields[k] = Long.parseLong(new String(b, at + 6 + k * 8, 8, StandardCharsets.US_ASCII), 16);
      int nameSize = (int) e.fields[11], fileSize = (int) e.fields[6];
      e.name = new String(b, at + 110, nameSize - 1, StandardCharsets.UTF_8);
      int dataAt = align(at + 110 + nameSize, 4);
      e.data = Arrays.copyOfRange(b, dataAt, dataAt + fileSize);
      at = align(dataAt + fileSize, 4);
      out.add(e);
      if (e.name.equals("TRAILER!!!")) break;
    }
    return out;
  }

  static byte[] writeCpio(List<Entry> entries) throws IOException {
    ByteArrayOutputStream o = new ByteArrayOutputStream();
    for (Entry e : entries) {
      byte[] name = (e.name + "\0").getBytes(StandardCharsets.UTF_8);
      e.fields[11] = name.length;
      e.fields[6] = e.data.length;
      StringBuilder hdr = new StringBuilder("070701");
      for (long f : e.fields) hdr.append(String.format("%08x", f));
      o.write(hdr.toString().getBytes(StandardCharsets.US_ASCII));
      o.write(name);
      while (o.size() % 4 != 0) o.write(0);
      o.write(e.data);
      while (o.size() % 4 != 0) o.write(0);
    }
    return o.toByteArray();
  }
}
