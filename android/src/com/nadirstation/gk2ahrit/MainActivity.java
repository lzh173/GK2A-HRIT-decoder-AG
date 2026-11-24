package com.nadirstation.gk2ahrit;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.Cursor;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.provider.DocumentsContract;

import org.qtproject.qt.android.bindings.QtActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * QtActivity subclass that owns the Storage Access Framework directory picker and the export of
 * the decoder's staging tree into the user-selected document tree.
 */
public class MainActivity extends QtActivity {
    private static final int REQ_OUTPUT_TREE = 0x4e21;
    private static final String USB_PERMISSION = "com.nadirstation.gk2ahrit.USB_PERMISSION";

    private static native void nativeTreeSelected(String uri, int flags);

    private boolean usbReceiverRegistered = false;
    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // Permission dialogs update UsbManager's per-device grant; the next refresh/open sees it.
        }
    };

    public void requestSdrPermissions() {
        UsbManager manager = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (manager == null) return;
        if (!usbReceiverRegistered) {
            IntentFilter filter = new IntentFilter(USB_PERMISSION);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                registerReceiver(usbReceiver, filter);
            }
            usbReceiverRegistered = true;
        }
        Intent intent = new Intent(USB_PERMISSION);
        PendingIntent pending = PendingIntent.getBroadcast(this, 0, intent, PendingIntent.FLAG_IMMUTABLE);
        for (UsbDevice device : manager.getDeviceList().values()) {
            if (isSdr(device) && !manager.hasPermission(device)) {
                manager.requestPermission(device, pending);
            }
        }
    }

    private boolean isSdr(UsbDevice device) {
        int vid = device.getVendorId();
        int pid = device.getProductId();
        if (vid == 0x1d50 && (pid == 0x604b || pid == 0x6089 || pid == 0xcc15)) return true;
        if (vid == 0x0bda && (pid == 0x2832 || pid == 0x2838)) return true;
        return vid == 0x0413 && (pid == 0x6680 || pid == 0x6f0f);
    }

    @Override
    protected void onDestroy() {
        if (usbReceiverRegistered) {
            unregisterReceiver(usbReceiver);
            usbReceiverRegistered = false;
        }
        super.onDestroy();
    }

    public void pickOutputTree() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, REQ_OUTPUT_TREE);
    }

    public String treeDisplayName(String uri) {
        try {
            Uri tree = Uri.parse(uri);
            String id = DocumentsContract.getTreeDocumentId(tree);
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
            String[] columns = {DocumentsContract.Document.COLUMN_DISPLAY_NAME};
            try (Cursor cursor = getContentResolver().query(doc, columns, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
            }
            return id;
        } catch (Exception e) {
            return uri;
        }
    }

    public boolean exportToTree(String treeUri, String localDir) {
        try {
            Uri tree = Uri.parse(treeUri);
            ContentResolver resolver = getContentResolver();
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, rootId);
            File source = new File(localDir);
            if (!source.isDirectory()) return false;
            copyDirectory(resolver, root, source);
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    private void copyDirectory(ContentResolver resolver, Uri parent, File directory) throws Exception {
        File[] children = directory.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (child.isDirectory()) {
                Uri sub = createDocument(resolver, parent, DocumentsContract.Document.MIME_TYPE_DIR, child.getName());
                if (sub != null) copyDirectory(resolver, sub, child);
                continue;
            }
            Uri document = createDocument(resolver, parent, mimeFor(child.getName()), child.getName());
            if (document == null) continue;
            try (InputStream in = new FileInputStream(child);
                 OutputStream out = resolver.openOutputStream(document)) {
                if (out == null) continue;
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) > 0) out.write(buffer, 0, count);
                out.flush();
            }
        }
    }

    private Uri createDocument(ContentResolver resolver, Uri parent, String mime, String name) throws Exception {
        Uri document = DocumentsContract.createDocument(resolver, parent, mime, name);
        if (document != null) return document;
        int dot = name.lastIndexOf('.');
        String base = dot > 0 ? name.substring(0, dot) : name;
        String extension = dot > 0 ? name.substring(dot) : "";
        for (int i = 1; i < 1000; ++i) {
            document = DocumentsContract.createDocument(resolver, parent, mime, base + " (" + i + ")" + extension);
            if (document != null) return document;
        }
        return null;
    }

    private String mimeFor(String name) {
        String lower = name.toLowerCase(Locale.US);
        if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
        if (lower.endsWith(".png")) return "image/png";
        if (lower.endsWith(".json")) return "application/json";
        if (lower.endsWith(".txt") || lower.endsWith(".log")) return "text/plain";
        return "application/octet-stream";
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_OUTPUT_TREE) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                Uri uri = data.getData();
                int flags = data.getFlags()
                        & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                try {
                    getContentResolver().takePersistableUriPermission(uri, flags);
                } catch (Exception ignored) {
                }
                nativeTreeSelected(uri.toString(), flags);
            } else {
                nativeTreeSelected("", 0);
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }
}
