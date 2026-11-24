// Link-time compatibility for imported decoder sources built for Android.
//
// libavc (core/src/imported/libavc) declares `inline int android_errorWriteLog(...)` under __ANDROID__. In C99 a
// plain `inline` definition is not an external definition, so unoptimised (debug) builds that do not inline the call
// fail to link. This provides the external definition; it is not exported from libconvertible_jni.so.
#include <android/log.h>

extern "C" int android_errorWriteLog(int tag, const char* sub_tag) {
  __android_log_print(ANDROID_LOG_ERROR, "convertible", "android_errorWriteLog(%x, %s)", tag, sub_tag ? sub_tag : "");
  return 0;
}
