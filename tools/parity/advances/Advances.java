// Advances: glyph advance widths as Android 4.2 lays text out (Paint and TextLayoutCache, with the
// bundled Roboto), measured on the Y1 itself. Rondo 2.0 positions text with this table, so its lines
// are exactly as wide as 1.0.x's: the old FreeType autohinter that Android 4.2 ships changes advances
// at small sizes, which no current FreeType reproduces (private/native-plan.md, N1 log).
//
// Run with app_process (tools/parity/advances/measure.sh): Advances <font.ttf> <out.bin> <px sizes...>
// Output: "RADV", u32 version, u32 sizes, u32 ranges, then (from, to) per range, the sizes as float,
// then per size and code point a float advance.
import android.graphics.Paint;
import android.graphics.Typeface;
import java.io.DataOutputStream;
import java.io.FileOutputStream;

public class Advances {
  static final int[][] RANGES = {
    {0x20, 0x7e}, {0xa0, 0x24f}, {0x259, 0x259}, {0x370, 0x3ff}, {0x400, 0x4ff}, {0x2000, 0x206f}, {0x20ac, 0x20ac},
    {0x2190, 0x2193}, {0x2122, 0x2122},
  };

  public static void main(String[] args) throws Exception {
    Typeface tf = Typeface.createFromFile(args[0]);
    Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    p.setTypeface(tf);
    if (args[1].equals("--pairs")) {  // <font> --pairs <out.bin>: every pair of common characters Android kerns
      p.setTextSize(100f);
      java.util.ArrayList<Integer> cps = new java.util.ArrayList<Integer>();
      for (int cp = 0x21; cp <= 0x7e; cp++) cps.add(cp);
      for (int cp = 0xa0; cp <= 0x17f; cp++) cps.add(cp);
      cps.add(0x2018); cps.add(0x2019); cps.add(0x201c); cps.add(0x201d); cps.add(0x2026);
      float[] single = new float[cps.size()];
      for (int i = 0; i < cps.size(); i++) single[i] = p.measureText(new String(Character.toChars(cps.get(i))));
      DataOutputStream out = new DataOutputStream(new FileOutputStream(args[2]));
      out.writeBytes("RKRN");
      int count = 0;
      java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
      DataOutputStream b = new DataOutputStream(body);
      for (int i = 0; i < cps.size(); i++)
        for (int j = 0; j < cps.size(); j++) {
          String s = new String(Character.toChars(cps.get(i))) + new String(Character.toChars(cps.get(j)));
          if (Math.abs(p.measureText(s) - single[i] - single[j]) >= 0.5f) { b.writeShort(cps.get(i)); b.writeShort(cps.get(j)); count++; }
        }
      out.writeInt(count);
      out.write(body.toByteArray());
      out.close();
      System.out.println(count + " kerned pairs");
      return;
    }
    if (args[1].equals("--measure")) {  // <font> --measure <px> <strings...>: widths and per-glyph advances
      p.setTextSize(Float.parseFloat(args[2]));
      for (int i = 3; i < args.length; i++) {
        float[] w = new float[args[i].length()];
        p.getTextWidths(args[i], w);
        StringBuilder sb = new StringBuilder(args[i] + " = " + p.measureText(args[i]) + ":");
        for (float x : w) sb.append(' ').append(x);
        System.out.println(sb);
      }
      return;
    }
    int n = args.length - 2;
    DataOutputStream out = new DataOutputStream(new FileOutputStream(args[1]));
    out.writeBytes("RADV");
    out.writeInt(1);
    out.writeInt(n);
    out.writeInt(RANGES.length);
    for (int[] r : RANGES) { out.writeInt(r[0]); out.writeInt(r[1]); }
    for (int i = 0; i < n; i++) out.writeFloat(Float.parseFloat(args[2 + i]));
    for (int i = 0; i < n; i++) {
      p.setTextSize(Float.parseFloat(args[2 + i]));
      for (int[] r : RANGES)
        for (int cp = r[0]; cp <= r[1]; cp++) out.writeFloat(p.measureText(new String(Character.toChars(cp))));
    }
    out.close();
    // is text kerned? a pair's width against its two glyphs'
    p.setTextSize(21f);
    String[] pairs = {"AV", "To", "Ay", "Fe", "Te", "Ye", "LT", "Wa", "11", "ff"};
    StringBuilder sb = new StringBuilder("pairs at 21 px (pair - a - b):");
    for (String s : pairs) {
      float d = p.measureText(s) - p.measureText(s.substring(0, 1)) - p.measureText(s.substring(1));
      sb.append(' ').append(s).append(' ').append(d);
    }
    System.out.println(sb);
    float[] w = new float[5];
    p.setTextSize(15.4f);
    p.getTextWidths("SONGS", w);
    System.out.println("SONGS at 15.4: " + w[0] + " " + w[1] + " " + w[2] + " " + w[3] + " " + w[4] + " total " + p.measureText("SONGS"));
  }
}
