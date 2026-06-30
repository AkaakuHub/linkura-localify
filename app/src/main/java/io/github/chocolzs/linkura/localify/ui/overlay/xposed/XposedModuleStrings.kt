package io.github.chocolzs.linkura.localify.ui.overlay.xposed

import android.content.res.XModuleResources
import io.github.chocolzs.linkura.localify.hookUtils.FilesChecker

object XposedModuleStrings {
    fun get(key: Int, vararg formatArgs: Any): String {
        val resources = XModuleResources.createInstance(FilesChecker.modulePath, null)
        return if (formatArgs.isEmpty()) {
            resources.getString(key)
        } else {
            resources.getString(key, *formatArgs)
        }
    }
}
