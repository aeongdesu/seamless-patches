package app.aeong.steamlink

import app.reseam.patch.CodeScope
import app.reseam.patch.PatchRuntime
import app.reseam.patch.Type
import app.reseam.patch.ValueRef
import app.reseam.patch.XmlDocument
import app.reseam.patch.XmlElement
import app.reseam.patch.addMethod
import app.reseam.patch.before
import app.reseam.patch.dex.AccessFlags
import app.reseam.patch.klass
import app.reseam.patch.method
import app.reseam.patch.patch
import app.reseam.patch.proto
import app.reseam.patch.types.EncodedVal

private const val STEAM_LINK_PACKAGE = "com.valvesoftware.steamlinkvr"
private const val DEFAULT_PACKAGE_NAME = "app.aeong.steamlink"
private const val VRLINK_ACTIVITY_CLASS = "com.valvesoftware.steamlink.VRLink"
private const val BUILD_CONFIG_CLASS = "com.valvesoftware.steamlink.BuildConfig"
private const val PAMIR_PREF_SUFFIX = ".pamir_pref"
private const val GET_SHARED_PREFERENCES_PROTO =
    "(Ljava/lang/String;I)Landroid/content/SharedPreferences;"

private val PACKAGE_NAME_PATTERN =
    Regex("^[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)+$")

private val COMPONENT_TAGS = listOf(
    "application",
    "activity",
    "activity-alias",
    "service",
    "receiver",
    "provider",
)

private val vrLinkActivity = klass(VRLINK_ACTIVITY_CLASS)

val changePackageName = patch(SteamLinkPatchStrings.changePackageName) {
    description(SteamLinkPatchStrings.changePackageNameDescription)
    compatibleWith(STEAM_LINK_PACKAGE)

    val packageNameOption = stringOption(
        key = "packageName",
        title = SteamLinkPatchStrings.optPackageNameTitle,
        description = SteamLinkPatchStrings.optPackageNameDescription,
        default = DEFAULT_PACKAGE_NAME,
    )

    execute {
        val newPackage = options[packageNameOption].trim()
        require(PACKAGE_NAME_PATTERN.matches(newPackage)) {
            "Invalid Android package name: '$newPackage'"
        }

        val currentPackage = manifest.packageName
            ?: error("AndroidManifest.xml is missing a package attribute")

        val oldPackages = linkedSetOf(STEAM_LINK_PACKAGE, currentPackage)
            .filter { it != newPackage }

        rewriteManifests(currentPackage, oldPackages, newPackage)
        rewriteResourcePackages(newPackage)
        rewriteDexPackageReferences(oldPackages, newPackage)
        remapVrLinkSharedPreferences(oldPackages, newPackage)
    }
}

private fun PatchRuntime.rewriteManifests(
    currentPackage: String,
    oldPackages: List<String>,
    newPackage: String,
) {
    for (component in manifest.components()) {
        manifest.component(component).edit {
            val componentPackage = root["package"] ?: currentPackage
            if (componentPackage != newPackage) {
                qualifyRelativeComponentNames(componentPackage)
                root["package"] = newPackage
            }
            rewritePackageScopedAttributes(oldPackages, newPackage)
        }
    }
}

private fun XmlDocument.qualifyRelativeComponentNames(oldPackage: String) {
    for (tag in COMPONENT_TAGS) {
        for (element in findByTag(tag)) {
            qualifyClassAttribute(element, "android:name", oldPackage)
            if (tag == "activity-alias") {
                qualifyClassAttribute(element, "android:targetActivity", oldPackage)
            }
        }
    }
}

private fun qualifyClassAttribute(element: XmlElement, attr: String, oldPackage: String) {
    val rawName = element[attr] ?: return
    val qualified = when {
        rawName.startsWith(".") -> oldPackage + rawName
        !rawName.contains('.') -> "$oldPackage.$rawName"
        else -> return
    }
    element[attr] = qualified
}

private fun XmlDocument.rewritePackageScopedAttributes(
    oldPackages: List<String>,
    newPackage: String,
) {
    if (oldPackages.isEmpty()) return

    for (provider in findByTag("provider")) {
        val authorities = provider["android:authorities"] ?: continue
        val updated = authorities.split(';').joinToString(";") { entry ->
            replacePackagePrefix(entry, oldPackages, newPackage)
        }
        if (updated != authorities) {
            provider["android:authorities"] = updated
        }
    }

    for (tag in listOf("permission", "uses-permission")) {
        for (element in findByTag(tag)) {
            val name = element["android:name"] ?: continue
            val updated = replacePackagePrefix(name, oldPackages, newPackage)
            if (updated != name) {
                element["android:name"] = updated
            }
        }
    }
}

private fun replacePackagePrefix(
    value: String,
    oldPackages: List<String>,
    newPackage: String,
): String {
    for (oldPkg in oldPackages) {
        if (value == oldPkg) return newPackage
        if (value.startsWith("$oldPkg.")) {
            return newPackage + value.removePrefix(oldPkg)
        }
    }
    return value
}

private fun PatchRuntime.rewriteResourcePackages(newPackage: String) {
    for (component in resources.components()) {
        if ("resources.arsc" in files.component(component).list()) {
            resources.component(component).setPackageName(newPackage)
        }
    }
}

private fun PatchRuntime.rewriteDexPackageReferences(
    oldPackages: List<String>,
    newPackage: String,
) {
    var exactReplacements = 0
    var prefixedReplacements = 0

    for (oldPkg in oldPackages) {
        exactReplacements += bytecode.replaceAllStrings(oldPkg, newPackage)
        prefixedReplacements += bytecode.replaceStringsStartingWith("$oldPkg.") { value ->
            newPackage + value.removePrefix(oldPkg)
        }
    }

    val buildConfig = bytecode.findClass(BUILD_CONFIG_CLASS)
    if (buildConfig?.field("APPLICATION_ID") != null) {
        buildConfig.setStaticFieldValue("APPLICATION_ID", EncodedVal.StringVal(newPackage))
    }

    log.info(
        "Rewrote Steam Link DEX package references to $newPackage " +
            "(exact=$exactReplacements, prefixed=$prefixedReplacements)"
    )
}

private fun PatchRuntime.remapVrLinkSharedPreferences(
    oldPackages: List<String>,
    newPackage: String,
) {
    val oldPrefNames = oldPackages.map { "$it$PAMIR_PREF_SUFFIX" }
    if (oldPrefNames.isEmpty()) return

    val newPrefName = "$newPackage$PAMIR_PREF_SUFFIX"
    val existingOverride = vrLinkActivity.classDef.method(
        "getSharedPreferences",
        GET_SHARED_PREFERENCES_PROTO,
    )

    if (existingOverride != null) {
        vrLinkActivity.method("getSharedPreferences") {
            params(Type.String, Type.Int)
        }.before {
            remapPamirPreferenceName(param(0), oldPrefNames, newPrefName)
        }
    } else {
        vrLinkActivity.addMethod(
            name = "getSharedPreferences",
            proto = GET_SHARED_PREFERENCES_PROTO,
            accessFlags = AccessFlags.PUBLIC,
        ) {
            val prefName = param(0)
            val mode = param(1)
            remapPamirPreferenceName(prefName, oldPrefNames, newPrefName)
            returnValue(
                thisObject.callSuper(
                    "android.app.NativeActivity",
                    "getSharedPreferences",
                    GET_SHARED_PREFERENCES_PROTO,
                    prefName,
                    mode,
                ),
            )
        }
    }
}

private fun CodeScope.remapPamirPreferenceName(
    prefName: ValueRef,
    oldPrefNames: List<String>,
    newPrefName: String,
) {
    for (oldPrefName in oldPrefNames) {
        val isMatch = string(oldPrefName).callVirtual(
            "java.lang.String",
            "equals",
            proto(Type.Boolean, Type.Object),
            prefName,
        )
        whenTrue(isMatch) {
            prefName.assign(string(newPrefName))
        }
    }
}
