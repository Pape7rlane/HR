package com.hsrbox.tool;

/**
 * Whether LSPosed loaded HSRBox into its own app process: Entry sets these fields when the module's scope includes HSRBox
 * itself (handleLoadPackage for com.hsrbox.tool). Without that they keep their defaults, so the app reads "not active".
 */
public final class ModuleStatus {
    private ModuleStatus() {}
    public static volatile boolean active;
    /** XposedBridge.getXposedVersion() of the framework that loaded the module. */
    public static volatile int api;
}
