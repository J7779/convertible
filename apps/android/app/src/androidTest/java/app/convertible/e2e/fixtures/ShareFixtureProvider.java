package app.convertible.e2e.fixtures;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Arrays;
import java.util.List;

/**
 * Stands in for "another app" that shares files with Convertible (ACTION_SEND / SEND_MULTIPLE) or opens them with
 * it (ACTION_VIEW): {@code content://app.convertible.test.share/<fixture name>}.
 *
 * <p>It also exposes test hooks for the documents {@link FixtureDocumentsProvider} received (same package, same
 * files), because that provider is guarded by MANAGE_DOCUMENTS, which tests do not hold: {@code call("reset")}
 * deletes them, {@code call("list")} names them, and {@code saved/<name>} reads one.
 */
public final class ShareFixtureProvider extends ContentProvider {
    public static final String AUTHORITY = "app.convertible.test.share";
    public static final String METHOD_RESET = "reset";
    public static final String METHOD_LIST = "list";
    public static final String KEY_NAMES = "names";

    public static Uri uri(String name) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(name).build();
    }

    /** Read-only view of a document the storage provider received. */
    public static Uri savedUri(String name) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("saved").appendPath(name).build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        String name = nameOf(uri);
        String[] columns = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor cursor = new MatrixCursor(columns);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = name;
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = Fixtures.size(name);
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        return Fixtures.mime(nameOf(uri));
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read-only");
        List<String> segments = uri.getPathSegments();
        if (segments.size() == 2 && "saved".equals(segments.get(0))) {
            File file = new File(FixtureFiles.savedDir(getContext()), segments.get(1));
            if (!file.isFile()) throw new FileNotFoundException(uri.toString());
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        return ParcelFileDescriptor.open(FixtureFiles.fixture(getContext(), nameOf(uri)), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        File saved = FixtureFiles.savedDir(getContext());
        switch (method) {
            case METHOD_RESET:
                FixtureFiles.deleteRecursively(saved);
                return new Bundle();
            case METHOD_LIST: {
                String[] names = saved.list();
                if (names == null) names = new String[0];
                Arrays.sort(names);
                Bundle bundle = new Bundle();
                bundle.putStringArray(KEY_NAMES, names);
                return bundle;
            }
            default:
                return null;
        }
    }

    private static String nameOf(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null || !Fixtures.ALL.contains(name)) throw new IllegalArgumentException("unknown fixture " + uri);
        return name;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
