package app.convertible.e2e.fixtures;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Arrays;
import java.util.Locale;

/**
 * A storage root shown in the system file picker and save dialog ("Convertible E2E"). It lists the fixtures
 * (read-only) and accepts new documents, which the tests read back to check what the app saved.
 *
 * <p>Like the platform's own storage provider, it appends the extension of the requested MIME type when the name does
 * not already carry an extension of that type, so a wrong MIME type from the app surfaces as a wrong file name.
 */
public final class FixtureDocumentsProvider extends DocumentsProvider {
    public static final String AUTHORITY = "app.convertible.test.documents";
    public static final String ROOT_ID = "fixtures";
    public static final String ROOT_DOC = "root";
    public static final String ROOT_TITLE = "Convertible E2E";
    static final String FIXTURE = "fixture:";
    static final String SAVED = "saved:";

    private static final String[] ROOT_COLUMNS = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY, Root.COLUMN_FLAGS,
        Root.COLUMN_MIME_TYPES, Root.COLUMN_AVAILABLE_BYTES, Root.COLUMN_ICON,
    };
    private static final String[] DOC_COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
        Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED,
    };

    /** The document URI of a saved document (what ACTION_CREATE_DOCUMENT hands back to the app). */
    public static Uri savedDocumentUri(String name) {
        return DocumentsContract.buildDocumentUri(AUTHORITY, SAVED + name);
    }

    private File savedDir() {
        return FixtureFiles.savedDir(getContext());
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        cursor.newRow()
            .add(Root.COLUMN_ROOT_ID, ROOT_ID)
            .add(Root.COLUMN_DOCUMENT_ID, ROOT_DOC)
            .add(Root.COLUMN_TITLE, ROOT_TITLE)
            .add(Root.COLUMN_SUMMARY, "Test files")
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_MIME_TYPES, "*/*")
            .add(Root.COLUMN_AVAILABLE_BYTES, savedDir().getUsableSpace())
            .add(Root.COLUMN_ICON, android.R.drawable.ic_menu_save);
        return cursor;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        addDocument(cursor, documentId);
        return cursor;
    }

    @Override
    public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        if (ROOT_DOC.equals(parentId)) {
            for (String name : Fixtures.ALL) addDocument(cursor, FIXTURE + name);
            String[] saved = savedDir().list();
            if (saved != null) {
                Arrays.sort(saved);
                for (String name : saved) addDocument(cursor, SAVED + name);
            }
        }
        cursor.setNotificationUri(getContext().getContentResolver(), DocumentsContract.buildChildDocumentsUri(AUTHORITY, ROOT_DOC));
        return cursor;
    }

    private void addDocument(MatrixCursor cursor, String id) throws FileNotFoundException {
        MatrixCursor.RowBuilder row = cursor.newRow();
        if (ROOT_DOC.equals(id)) {
            row.add(Document.COLUMN_DOCUMENT_ID, ROOT_DOC)
                .add(Document.COLUMN_DISPLAY_NAME, ROOT_TITLE)
                .add(Document.COLUMN_MIME_TYPE, Document.MIME_TYPE_DIR)
                .add(Document.COLUMN_FLAGS, Document.FLAG_DIR_SUPPORTS_CREATE);
        } else if (id.startsWith(FIXTURE)) {
            String name = id.substring(FIXTURE.length());
            if (!Fixtures.ALL.contains(name)) throw new FileNotFoundException(id);
            row.add(Document.COLUMN_DOCUMENT_ID, id)
                .add(Document.COLUMN_DISPLAY_NAME, name)
                .add(Document.COLUMN_MIME_TYPE, Fixtures.mime(name))
                .add(Document.COLUMN_FLAGS, 0)
                .add(Document.COLUMN_SIZE, Fixtures.size(name));
        } else if (id.startsWith(SAVED)) {
            File file = new File(savedDir(), id.substring(SAVED.length()));
            if (!file.isFile()) throw new FileNotFoundException(id);
            row.add(Document.COLUMN_DOCUMENT_ID, id)
                .add(Document.COLUMN_DISPLAY_NAME, file.getName())
                .add(Document.COLUMN_MIME_TYPE, mimeOf(file.getName()))
                .add(Document.COLUMN_FLAGS, Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE)
                .add(Document.COLUMN_SIZE, file.length());
        } else {
            throw new FileNotFoundException(id);
        }
        row.add(Document.COLUMN_LAST_MODIFIED, 0L);
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal) throws FileNotFoundException {
        if (documentId.startsWith(FIXTURE)) {
            if (!"r".equals(mode)) throw new SecurityException("fixtures are read-only");
            return ParcelFileDescriptor.open(FixtureFiles.fixture(getContext(), documentId.substring(FIXTURE.length())),
                ParcelFileDescriptor.MODE_READ_ONLY);
        }
        if (documentId.startsWith(SAVED)) {
            File file = new File(savedDir(), documentId.substring(SAVED.length()));
            if (!file.exists()) throw new FileNotFoundException(documentId);
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
        }
        throw new FileNotFoundException(documentId);
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        if (!ROOT_DOC.equals(parentDocumentId)) throw new FileNotFoundException(parentDocumentId);
        if (Document.MIME_TYPE_DIR.equals(mimeType)) throw new UnsupportedOperationException("no folders");
        String name = uniqueName(withExtensionFor(mimeType, displayName.replace('/', '_')));
        try {
            if (!new File(savedDir(), name).createNewFile()) throw new FileNotFoundException("exists: " + name);
        } catch (IOException e) {
            throw new FileNotFoundException(e.toString());
        }
        getContext().getContentResolver().notifyChange(DocumentsContract.buildChildDocumentsUri(AUTHORITY, ROOT_DOC), null);
        return SAVED + name;
    }

    @Override
    public void deleteDocument(String documentId) {
        if (!documentId.startsWith(SAVED)) throw new UnsupportedOperationException("read-only");
        //noinspection ResultOfMethodCallIgnored
        new File(savedDir(), documentId.substring(SAVED.length())).delete();
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        return ROOT_DOC.equals(parentDocumentId) && !ROOT_DOC.equals(documentId);
    }

    @Override
    public String getDocumentType(String documentId) {
        if (ROOT_DOC.equals(documentId)) return Document.MIME_TYPE_DIR;
        if (documentId.startsWith(FIXTURE)) return Fixtures.mime(documentId.substring(FIXTURE.length()));
        return mimeOf(documentId.substring(SAVED.length()));
    }

    private String uniqueName(String name) {
        if (!new File(savedDir(), name).exists()) return name;
        int dot = name.lastIndexOf('.');
        if (dot <= 0) dot = name.length();
        for (int i = 1; ; i++) {
            String candidate = name.substring(0, dot) + " (" + i + ")" + name.substring(dot);
            if (!new File(savedDir(), candidate).exists()) return candidate;
        }
    }

    private static String extension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(dot + 1).toLowerCase(Locale.ROOT) : "";
    }

    private static String mimeOf(String name) {
        String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension(name));
        return mime != null ? mime : "application/octet-stream";
    }

    private static String withExtensionFor(String mime, String name) {
        if ("application/octet-stream".equals(mime) || mime.endsWith("/*")) return name;
        String ext = extension(name);
        if (!ext.isEmpty() && mime.equals(MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext))) return name;
        String wanted = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime);
        return wanted == null ? name : name + "." + wanted;
    }
}
