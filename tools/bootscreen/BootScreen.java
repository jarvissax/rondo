import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.geom.Arc2D;
import java.awt.geom.Ellipse2D;
import java.awt.geom.Line2D;
import java.awt.geom.Path2D;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import javax.imageio.ImageIO;

// Rondo's boot screen (it replaces the Rockbox logo; the project is named Rondo, a
// piece of music that keeps circling back to its theme). The mark is the click wheel as a white
// ring with one red segment, the playhead, and a red play mark at its heart; the wordmark
// "rondo" is drawn from the same round strokes, so no font is needed. One design, three outputs:
//   bootanimation.zip  Android's boot animation (480x360, 20 fps): the playhead starts at the
//                      top, eases into motion over one turn, then circles until the launcher is up
//   logo.png / logo.bin  the static logo the bootloader and kernel show first; it is frame 0 of
//                      the animation, so the hand-over is seamless. logo.bin is the original
//                      MediaTek logo partition with images 0 and N-1 replaced (the charging
//                      animation in between is kept).
// Run with JDK 11+ from the repo root:
//   java tools/bootscreen/BootScreen.java <out-dir> [original-logo.bin]
public class BootScreen {
    static final int W = 480, H = 360, FPS = 20;
    static final int INTRO = 60, LOOP = 30; // intro: one accelerating turn; loop: one turn in 1.5 s
    static final double CX = 240, CY = 144, R = 58, RING = 13; // ring center line radius and stroke
    static final double SEGMENT = 44, GAP = 18; // playhead length and the gaps around it, degrees
    static final double REST = 52; // where the playhead rests (at 12 o'clock the mark read as a power symbol)
    static final Color WHITE = new Color(0xf5, 0xf5, 0xf7);
    static final Color RED = new Color(0xff, 0x3b, 0x4f);

    public static void main(String[] args) throws Exception {
        File out = new File(args.length > 0 ? args[0] : "bootscreen-out");
        out.mkdirs();
        byte[][] intro = new byte[INTRO][], loop = new byte[LOOP][];
        // intro: angle = a*i^2 with one full turn at i = INTRO, ending at the loop's speed
        double a = 360.0 / ((double) INTRO * INTRO);
        for (int i = 0; i < INTRO; i++) intro[i] = png(frame(REST + a * i * i));
        for (int i = 0; i < LOOP; i++) loop[i] = png(frame(REST + i * 360.0 / LOOP));
        writeZip(new File(out, "bootanimation.zip"), intro, loop);

        BufferedImage logo = frame(REST);
        ImageIO.write(logo, "png", new File(out, "logo.png"));
        ImageIO.write(frame(REST + 150), "png", new File(out, "preview-turning.png"));
        if (args.length > 1) {
            byte[] packed = repackLogo(Files.readAllBytes(new File(args[1]).toPath()), rgb565(logo));
            Files.write(new File(out, "logo.bin").toPath(), packed);
        }
        System.out.println("wrote " + out.getPath());
    }

    // head: where the playhead's middle is, degrees clockwise from 12 o'clock
    static BufferedImage frame(double head) {
        BufferedImage img = new BufferedImage(W, H, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_RENDERING, RenderingHints.VALUE_RENDER_QUALITY);
        g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE);
        g.setColor(Color.BLACK);
        g.fillRect(0, 0, W, H);

        // the wheel: white ring with a gap either side of the red playhead
        BasicStroke ring = new BasicStroke((float) RING, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND);
        g.setStroke(ring);
        g.setColor(WHITE);
        g.draw(arc(CX, CY, R, head + SEGMENT / 2 + GAP, 360 - SEGMENT - 2 * GAP));
        g.setColor(RED);
        g.draw(arc(CX, CY, R, head - SEGMENT / 2, SEGMENT));

        // play mark, nudged right to sit optically in the middle
        Path2D play = new Path2D.Double();
        play.moveTo(CX - 9, CY - 15);
        play.lineTo(CX - 9, CY + 15);
        play.lineTo(CX + 16, CY);
        play.closePath();
        g.setColor(RED);
        g.fill(play);
        g.setStroke(new BasicStroke(3f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));
        g.draw(play); // rounds the corners a little

        wordmark(g, 278);
        g.dispose();
        return img;
    }

    // an arc of the circle (cx, cy, r) from `start` sweeping `extent` degrees clockwise, both
    // measured from 12 o'clock (Java2D counts counter-clockwise from 3 o'clock)
    static Arc2D arc(double cx, double cy, double r, double start, double extent) {
        return new Arc2D.Double(cx - r, cy - r, 2 * r, 2 * r, 90 - start, -extent, Arc2D.OPEN);
    }

    // "rondo" in round strokes: x-height X on the stroke center line, stroke S, baseline y
    static void wordmark(Graphics2D g, double baseline) {
        double X = 25, S = 7.2, gap = 11.5, rr = X / 2;
        double[] widths = {X * 0.62, X, X, X, X}; // r o n d o
        double total = 0;
        for (double w : widths) total += w;
        total += gap * (widths.length - 1.45);
        double x = CX - total / 2;
        double top = baseline - X;
        g.setColor(WHITE);
        g.setStroke(new BasicStroke((float) S, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));

        // r: stem, and the shoulder of an n cut short
        g.draw(new Line2D.Double(x, baseline, x, top));
        g.draw(arc(x + rr, top + rr, rr, 270, 75));
        x += widths[0] + gap * 0.55; // the r's open side needs less room
        // o
        g.draw(new Ellipse2D.Double(x, top, X, X));
        x += widths[1] + gap;
        // n: stem, arch, leg
        Path2D n = new Path2D.Double();
        n.moveTo(x, baseline);
        n.lineTo(x, top);
        n.moveTo(x, top + rr);
        n.append(arc(x + rr, top + rr, rr, 270, 180), true);
        n.lineTo(x + X, baseline);
        g.draw(n);
        x += widths[2] + gap;
        // d: bowl and a tall stem on the right
        g.draw(new Ellipse2D.Double(x, top, X, X));
        g.draw(new Line2D.Double(x + X, baseline, x + X, top - X * 0.62));
        x += widths[3] + gap;
        // o
        g.draw(new Ellipse2D.Double(x, top, X, X));
    }

    static byte[] png(BufferedImage img) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        ImageIO.write(img, "png", b);
        return b.toByteArray();
    }

    // bootanimation.zip must be stored, not deflated
    static void writeZip(File f, byte[][] intro, byte[][] loop) throws Exception {
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(f))) {
            z.setMethod(ZipOutputStream.STORED);
            stored(z, "desc.txt", (W + " " + H + " " + FPS + "\np 1 0 part0\np 0 0 part1\n").getBytes("US-ASCII"));
            for (int i = 0; i < intro.length; i++) stored(z, String.format("part0/%03d.png", i), intro[i]);
            for (int i = 0; i < loop.length; i++) stored(z, String.format("part1/%03d.png", i), loop[i]);
        }
    }

    static void stored(ZipOutputStream z, String name, byte[] data) throws Exception {
        ZipEntry e = new ZipEntry(name);
        CRC32 crc = new CRC32();
        crc.update(data);
        e.setMethod(ZipEntry.STORED);
        e.setSize(data.length);
        e.setCompressedSize(data.length);
        e.setCrc(crc.getValue());
        z.putNextEntry(e);
        z.write(data);
        z.closeEntry();
    }

    // little-endian RGB565 with 4x4 ordered dithering (soft edges band without it)
    static byte[] rgb565(BufferedImage img) {
        int[] bayer = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        ByteBuffer b = ByteBuffer.allocate(W * H * 2).order(ByteOrder.LITTLE_ENDIAN);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int p = img.getRGB(x, y);
                double d = bayer[(y & 3) * 4 + (x & 3)] / 16.0 - 0.5;
                int r = clamp(((p >> 16) & 0xff) + d * 8), gr = clamp(((p >> 8) & 0xff) + d * 4), bl = clamp((p & 0xff) + d * 8);
                b.putShort((short) (((r >> 3) << 11) | ((gr >> 2) << 5) | (bl >> 3)));
            }
        }
        return b.array();
    }

    static int clamp(double v) {
        return (int) Math.max(0, Math.min(255, Math.round(v)));
    }

    // MediaTek logo partition: 512-byte header (size at offset 4, "LOGO" at 8), then
    // [count][body size][offset x count] and one zlib stream per image.
    static byte[] repackLogo(byte[] orig, byte[] raw) {
        ByteBuffer o = ByteBuffer.wrap(orig).order(ByteOrder.LITTLE_ENDIAN);
        if (o.getInt(8) != 0x4f474f4c) throw new IllegalArgumentException("not a MediaTek logo partition");
        int n = o.getInt(512), total = o.getInt(516);
        byte[][] images = new byte[n][];
        for (int i = 0; i < n; i++) {
            int start = o.getInt(520 + 4 * i), end = i + 1 < n ? o.getInt(520 + 4 * (i + 1)) : total;
            images[i] = java.util.Arrays.copyOfRange(orig, 512 + start, 512 + end);
        }
        byte[] mine = zlib(raw);
        images[0] = mine;
        images[n - 1] = mine;
        int body = 8 + 4 * n;
        for (byte[] im : images) body += im.length;
        ByteBuffer out = ByteBuffer.allocate(512 + body).order(ByteOrder.LITTLE_ENDIAN);
        out.put(orig, 0, 512);
        out.putInt(4, body);
        out.position(512);
        out.putInt(n).putInt(body);
        int at = 8 + 4 * n;
        for (byte[] im : images) {
            out.putInt(at);
            at += im.length;
        }
        for (byte[] im : images) out.put(im);
        return out.array();
    }

    static byte[] zlib(byte[] data) {
        Deflater d = new Deflater(Deflater.BEST_COMPRESSION);
        d.setInput(data);
        d.finish();
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        while (!d.finished()) b.write(buf, 0, d.deflate(buf));
        return b.toByteArray();
    }
}
