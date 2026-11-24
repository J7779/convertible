package app.convertible.e2e.fixtures;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;
import java.util.Random;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Deterministic test files. The same generator runs in the fixture providers (in the test package's own process,
 * serving the files to the app and to the system picker) and in the tests (computing the expected bytes), so nothing
 * is stored in git and nothing depends on what happens to be on the device.
 *
 * <p>Plain Java on purpose: the providers run in the test APK's process, which does not contain the Kotlin standard
 * library (the test APK leaves dependencies it shares with the app to the app APK).
 */
public final class Fixtures {
    public static final String NOTE = "note.txt";
    public static final String NOTE_2 = "shopping list.txt";
    public static final String NOTE_3 = "poem.txt";
    public static final String DATA = "sensor-data.bin";
    public static final String ENCODED = "message.b64";
    public static final String LARGE = "large-capture.bin";
    public static final String LARGE_2 = "large-capture-2.bin";
    public static final String PNG = "pixel-art.png";
    public static final String WAV = "tone.wav";
    public static final String ZIP = "bundle.zip";

    /** Size of the large fixtures: converting one to a checksum report takes several seconds on an emulator. */
    public static final long LARGE_SIZE = 96L * 1024 * 1024;

    /** Text with accented, CJK and supplementary characters (exercises the UTF-8 / UTF-16 paths). */
    public static final String NOTE_TEXT = "Convertible end-to-end ✓\nnaïve café — Ünïcödé, "
            + "日本語, emoji 😀🎉\nlast line\n";

    /** Every fixture, in the order the providers list them. */
    public static final List<String> ALL = Arrays.asList(NOTE, NOTE_2, NOTE_3, DATA, ENCODED, PNG, WAV, ZIP, LARGE, LARGE_2);

    private Fixtures() {}

    public static boolean isLarge(String name) {
        return LARGE.equals(name) || LARGE_2.equals(name);
    }

    public static String mime(String name) {
        if (name.endsWith(".txt")) return "text/plain";
        if (name.endsWith(".png")) return "image/png";
        if (name.endsWith(".wav")) return "audio/wav";
        if (name.endsWith(".zip")) return "application/zip";
        return "application/octet-stream";
    }

    public static long size(String name) {
        return isLarge(name) ? LARGE_SIZE : bytes(name).length;
    }

    /** The full contents of a fixture (large ones are materialised; prefer {@link #write} for those). */
    public static byte[] bytes(String name) {
        switch (name) {
            case NOTE:
                return NOTE_TEXT.getBytes(StandardCharsets.UTF_8);
            case NOTE_2:
                return "Milk\nEggs\nBread — wholemeal\nCoffee ☕\n".getBytes(StandardCharsets.UTF_8);
            case NOTE_3:
                return "Tyger Tyger, burning bright,\nIn the forests of the night;\n".getBytes(StandardCharsets.UTF_8);
            case DATA: {
                byte[] b = new byte[4096];
                new Random(42).nextBytes(b);
                b[0] = 0;
                b[1] = (byte) 0xFF;
                b[2] = 0x7F;
                b[3] = 0x10;
                return b;
            }
            case ENCODED: {
                byte[] raw = new byte[600];
                new Random(9).nextBytes(raw);
                return (Base64.getMimeEncoder().encodeToString(raw) + "\n").getBytes(StandardCharsets.US_ASCII);
            }
            case PNG:
                return png();
            case WAV:
                return wav();
            case ZIP:
                return zip();
            default:
                if (isLarge(name)) {
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    try {
                        write(name, out);
                    } catch (IOException e) {
                        throw new IllegalStateException(e);
                    }
                    return out.toByteArray();
                }
                throw new IllegalArgumentException("unknown fixture " + name);
        }
    }

    /** Streams a fixture; large ones are generated in 1 MiB chunks without holding them in memory. */
    public static void write(String name, OutputStream out) throws IOException {
        if (!isLarge(name)) {
            out.write(bytes(name));
            return;
        }
        Random random = new Random(LARGE.equals(name) ? 1234 : 5678);
        byte[] chunk = new byte[1 << 20];
        long left = LARGE_SIZE;
        while (left > 0) {
            random.nextBytes(chunk);
            int n = (int) Math.min(left, chunk.length);
            out.write(chunk, 0, n);
            left -= n;
        }
    }

    public static byte[] sha256(String name) {
        try {
            final MessageDigest md = MessageDigest.getInstance("SHA-256");
            write(name, new OutputStream() {
                @Override public void write(int b) { md.update((byte) b); }
                @Override public void write(byte[] b, int off, int len) { md.update(b, off, len); }
            });
            return md.digest();
        } catch (NoSuchAlgorithmException | IOException e) {
            throw new IllegalStateException(e);
        }
    }

    /** A 48x32 PNG with three coloured blocks, encoded by the platform. */
    private static byte[] png() {
        Bitmap bitmap = Bitmap.createBitmap(48, 32, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.rgb(20, 24, 31));
        Paint paint = new Paint();
        int[] colors = {Color.rgb(242, 99, 74), Color.rgb(60, 179, 113), Color.rgb(65, 105, 225)};
        for (int i = 0; i < colors.length; i++) {
            paint.setColor(colors[i]);
            canvas.drawRect(4f + i * 14f, 6f, 14f + i * 14f, 26f, paint);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }

    /** 0.25 s of a 440 Hz sine, 16-bit mono PCM at 8 kHz. */
    private static byte[] wav() {
        int rate = 8000;
        int samples = rate / 4;
        byte[] data = new byte[samples * 2];
        for (int i = 0; i < samples; i++) {
            int v = (int) (Math.sin(2 * Math.PI * 440 * i / rate) * 12000);
            data[2 * i] = (byte) v;
            data[2 * i + 1] = (byte) (v >> 8);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ascii(out, "RIFF");
        le32(out, 36 + data.length);
        ascii(out, "WAVE");
        ascii(out, "fmt ");
        le32(out, 16);
        le16(out, 1);
        le16(out, 1);
        le32(out, rate);
        le32(out, rate * 2);
        le16(out, 2);
        le16(out, 16);
        ascii(out, "data");
        le32(out, data.length);
        out.write(data, 0, data.length);
        return out.toByteArray();
    }

    private static byte[] zip() {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            ZipEntry readme = new ZipEntry("readme.txt");
            readme.setTime(0);
            z.putNextEntry(readme);
            z.write("Packed by the E2E fixtures.\n".getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
            ZipEntry csv = new ZipEntry("data/numbers.csv");
            csv.setTime(0);
            z.putNextEntry(csv);
            StringBuilder sb = new StringBuilder();
            for (int i = 1; i <= 50; i++) sb.append(i).append(',').append(i * i).append('\n');
            z.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        return out.toByteArray();
    }

    private static void ascii(ByteArrayOutputStream out, String s) {
        byte[] b = s.getBytes(StandardCharsets.US_ASCII);
        out.write(b, 0, b.length);
    }

    private static void le16(ByteArrayOutputStream out, int v) {
        out.write(v & 0xFF);
        out.write((v >> 8) & 0xFF);
    }

    private static void le32(ByteArrayOutputStream out, int v) {
        le16(out, v);
        le16(out, v >> 16);
    }
}
