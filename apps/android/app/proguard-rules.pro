# The JNI bridge registers its natives on this class by name and calls back into the progress sink by name.
-keep class app.convertible.core.NativeBridge {
    native <methods>;
}
-keep interface app.convertible.core.NativeProgressSink {
    boolean onProgress(double, java.lang.String);
}
