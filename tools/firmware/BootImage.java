// The Y1's boot image (Android's boot.img, MediaTek's 512-byte header on the ramdisk), edited for Rondo
// 2.0: init.rc starts Rondo, not the Java layer (firmware/build-native.sh).
//
//   java tools/firmware/BootImage.java in.img out.img
//
// In init.rc, zygote becomes disabled (no Java layer), and the "rondo" service runs /system/rondo/start;
// the kernel sets the panel up at 60 Hz (patchKernel).
// SurfaceFlinger still starts, to set the panel up as it always has, and the start script stops it
// before Rondo draws to the framebuffer itself. Everything else (the kernel apart from the panel's eight
// words, its command line, the other files of the ramdisk) stays byte for byte; the header's sizes and
// SHA-1 are made again.
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

    kernel = patchKernel(kernel);

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
  // tuner, and no apps to install or sign. drm stays: mediaserver's MetadataRetriever (a video's frame,
  // its artwork) waits for drmserver for good without it (Nov 10).
  static final java.util.List<String> UNUSED = java.util.Arrays.asList(
      "ccci_fsd", "ccci_mdinit", "gsm0710muxd", "ril-daemon", "mdlogger",  // the modem
      "agpsd", "matv", "DMAgent", "netdiag",                               // GPS, TV, device management, diagnostics
      "installd", "keystore");                                               // Android's apps

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

  // The panel at 60 Hz instead of 42.14, set by the kernel itself (Nov 9; native/platform/y1/display.cpp
  // has the measurements). The Y1's ST7701 on two DSI lanes keeps its porches (the panel's own settings
  // expect them) and the link runs faster: the MIPI PLL's divider 4 -> 2 and a fractional multiplier,
  // 43.197 (about 300 Mb/s a lane, 60.00 Hz measured), and the D-PHY timings worked out by MediaTek's
  // own formulas for it.
  // The kernel programs the PLL from the panel driver's parameters at every wake, so they are changed in
  // the kernel, at three places, each checked word for word first:
  // - st7701_hvga_dsi_vdo_boe's lcm_get_params: pll_div1 2 -> 1, and fbk_div 15 -> 0x2B32610A, the PLL's
  //   whole multiplier word with 10 in its low byte (stores of zeros the memset already made give way)
  // - DSI_PHY_clk_setting: a small fbk_div is written as before (fbk_div << 26, the other panel this
  //   kernel knows), a large one as the multiplier word itself (with its fraction)
  // - DSI_PHY_TIMCONFIG: fbk_div read as its low byte (10), so the timings are worked out for the clock
  static final int TEXT = 0xc0008000;
  static final int[][] KERNEL_PATCHES = {
      // lcm_get_params (st7701_hvga_dsi_vdo_boe)
      {0xc03ac3f8, 0xe58431a4, 0xe3a03001},  // str r3,[r4,#420] (pll_div1 = 2) -> mov r3,#1
      {0xc03ac3fc, 0xe3a0300f, 0xe58431a4},  // mov r3,#15 -> str r3,[r4,#420] (pll_div1 = 1)
      {0xc03ac404, 0xe5842130, 0xe306310a},  // str r2,[r4,#304] (0) -> movw r3,#0x610a
      {0xc03ac408, 0xe5842134, 0xe3423b32},  // str r2,[r4,#308] (0) -> movt r3,#0x2b32 (str r3,[r4,#428]: fbk_div)
      // DSI_PHY_clk_setting: MIPITX_DSI_PLL_CON2
      {0xc041f970, 0xe5943058, 0xe1b03422},  // ldr r3,[r4,#88] -> lsrs r3,r2,#8
      {0xc041f974, 0xe1a02102, 0x01a03d02},  // lsl r2,r2,#2 -> lsleq r3,r2,#26
      {0xc041f978, 0xe7de3c12, 0x11a03403},  // bfi r3,r2,#24,#7 -> lslne r3,r3,#8
      // DSI_PHY_TIMCONFIG
      {0xc041fd40, 0xe59051ac, 0xe5d051ac},  // ldr r5,[r0,#428] -> ldrb r5,[r0,#428]
  };

  // MediaTek's 512-byte header, then the zImage: its gzip'd Image is unpacked, patched and packed again
  // into exactly the same bytes (a shorter stream is padded with a file name in the gzip header, which
  // the kernel's decompressor skips), so the zImage's own sizes all stay right.
  static byte[] patchKernel(byte[] kernel) throws IOException {
    int at = -1;
    for (int i = 512; i + 3 < kernel.length && at < 0; i++)
      if ((kernel[i] & 0xff) == 0x1f && (kernel[i + 1] & 0xff) == 0x8b && kernel[i + 2] == 8 && (kernel[i + 3] & 0xe0) == 0) at = i;
    if (at < 0) throw new IOException("kernel: no gzip'd Image");
    int flags = kernel[at + 3] & 0xff, data = at + 10;
    if ((flags & 8) != 0) while (kernel[data++] != 0) {}
    if ((flags & ~8) != 0) throw new IOException("kernel: unexpected gzip flags " + flags);
    Inflater inf = new Inflater(true);
    inf.setInput(kernel, data, kernel.length - data);
    ByteArrayOutputStream raw = new ByteArrayOutputStream();
    byte[] buf = new byte[1 << 16];
    try {
      while (!inf.finished()) {
        int n = inf.inflate(buf);
        if (n == 0 && inf.needsInput()) throw new IOException("kernel: gzip stream cut short");
        raw.write(buf, 0, n);
      }
    } catch (DataFormatException e) {
      throw new IOException("kernel: " + e.getMessage());
    }
    int end = data + (int) inf.getBytesRead() + 8;  // the CRC-32 and the size follow the deflate data
    byte[] image = raw.toByteArray();
    ByteBuffer t = ByteBuffer.wrap(kernel, end - 4, 4).order(ByteOrder.LITTLE_ENDIAN);
    if (t.getInt() != image.length) throw new IOException("kernel: the gzip trailer's size doesn't match");
    ByteBuffer im = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
    for (int[] p : KERNEL_PATCHES) {
      int off = p[0] - TEXT, word = im.getInt(off);
      if (word == p[2]) continue;  // already patched
      if (word != p[1]) throw new IOException(String.format("kernel: 0x%08x holds 0x%08x, not 0x%08x: another kernel, left alone", p[0], word, p[1]));
      im.putInt(off, p[2]);
    }
    // packed again: the best of the deflate strategies, then padded to the original length
    byte[] best = null;
    for (int strategy : new int[] {Deflater.DEFAULT_STRATEGY, Deflater.FILTERED}) {
      Deflater d = new Deflater(Deflater.BEST_COMPRESSION, true);
      d.setStrategy(strategy);
      d.setInput(image);
      d.finish();
      ByteArrayOutputStream z = new ByteArrayOutputStream();
      while (!d.finished()) z.write(buf, 0, d.deflate(buf));
      byte[] b = z.toByteArray();
      if (best == null || b.length < best.length) best = b;
    }
    int room = end - at, need = 10 + best.length + 8;
    if (need > room) throw new IOException("kernel: packed again it is " + (need - room) + " bytes too long");
    int pad = room - need;  // a file name of pad - 1 characters and its NUL (none when pad is 0)
    ByteArrayOutputStream g = new ByteArrayOutputStream();
    g.write(new byte[] {0x1f, (byte) 0x8b, 8, (byte) (pad > 0 ? 8 : 0), 0, 0, 0, 0, 2, 3});
    for (int i = 0; i < pad - 1; i++) g.write('r');
    if (pad > 0) g.write(0);
    g.write(best);
    CRC32 crc = new CRC32();
    crc.update(image);
    g.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt((int) crc.getValue()).putInt(image.length).array());
    byte[] gz = g.toByteArray();
    if (gz.length != room) throw new IllegalStateException("kernel: packed to " + gz.length + " bytes, not " + room);
    byte[] out = kernel.clone();
    System.arraycopy(gz, 0, out, at, gz.length);
    System.out.println("kernel: the panel at 60 Hz (" + KERNEL_PATCHES.length + " words), packed again into the same " + room + " bytes ("
        + pad + " of padding)");
    return out;
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
