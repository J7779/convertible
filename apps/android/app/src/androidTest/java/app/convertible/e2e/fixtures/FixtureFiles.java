package app.convertible.e2e.fixtures;

import android.content.Context;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

/** Fixture files materialised on first use in the test package's own storage (outside the app under test). */
final class FixtureFiles {
    private FixtureFiles() {}

    static synchronized File fixture(Context context, String name) throws FileNotFoundException {
        if (!Fixtures.ALL.contains(name)) throw new FileNotFoundException("unknown fixture " + name);
        File dir = new File(context.getFilesDir(), "fixtures");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File file = new File(dir, name);
        if (!file.isFile() || file.length() != Fixtures.size(name)) {
            File tmp = new File(dir, name + ".tmp");
            try (OutputStream out = new BufferedOutputStream(new FileOutputStream(tmp), 1 << 20)) {
                Fixtures.write(name, out);
            } catch (IOException e) {
                throw new FileNotFoundException("cannot create fixture: " + e);
            }
            if (!tmp.renameTo(file)) throw new FileNotFoundException("cannot create fixture " + name);
        }
        return file;
    }

    /** Documents the storage provider received (from the app's save dialog). */
    static File savedDir(Context context) {
        File dir = new File(context.getFilesDir(), "saved");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir;
    }

    static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
