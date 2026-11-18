# Keep kotlinx.serialization serializers for LocalPresetRepository & JSON snapshots
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.SerializationKt
-keep,includedescriptorclasses class com.ascendance.dac.model.**$$serializer { *; }
-keepclassmembers class com.ascendance.dac.model.** {
    *** Companion;
}
