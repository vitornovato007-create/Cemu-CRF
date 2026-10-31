package info.cemu.cemu.settings

import android.os.Build
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.platform.LocalContext
import info.cemu.cemu.nativeinterface.NativeEmulation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.lifecycle.compose.dropUnlessResumed
import info.cemu.cemu.common.ui.components.Button
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.localization.tr

@Composable
fun SettingsHomeScreen(
    goToGeneralSettings: () -> Unit,
    goToInputSettings: () -> Unit,
    goToGraphicsSettings: () -> Unit,
    goToEmulatedUSBDevicesSettings: () -> Unit,
    goToAudioSettings: () -> Unit,
    goToAccountSettings: () -> Unit,
    goToOverlaySettings: () -> Unit,
    navigateBack: () -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var validationEnabled by remember { mutableStateOf(NativeEmulation.isVulkanValidationEnabled()) }
    val createLogDocument = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("text/plain")
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                try {
                    val report = buildString {
                        append(NativeEmulation.getVulkanDiagnosticLog())
                        append("\n\nAndroid device: ${Build.MANUFACTURER} ${Build.MODEL}")
                        append("\nAndroid API level: ${Build.VERSION.SDK_INT}\n")
                    }
                    val output = context.contentResolver.openOutputStream(uri)
                        ?: throw IllegalStateException("Unable to open the selected destination")
                    output.use { it.write(report.toByteArray(Charsets.UTF_8)) }
                    withContext(Dispatchers.Main) {
                        Toast.makeText(context, "Vulkan diagnostic log exported", Toast.LENGTH_LONG).show()
                    }
                } catch (exception: Exception) {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(
                            context,
                            "Failed to export Vulkan log: ${exception.message ?: "unknown error"}",
                            Toast.LENGTH_LONG
                        ).show()
                    }
                }
            }
        }
    }

    ScreenContent(
        appBarText = tr("Settings"),
        navigateBack = navigateBack,
    ) {
        Button(
            label = tr("General settings"),
            onClick = dropUnlessResumed(block = goToGeneralSettings)
        )
        Button(
            label = tr("Input settings"),
            onClick = dropUnlessResumed(block = goToInputSettings)
        )
        Button(
            label = tr("Graphics settings"),
            onClick = dropUnlessResumed(block = goToGraphicsSettings)
        )
        Button(
            label = tr("Audio settings"),
            onClick = dropUnlessResumed(block = goToAudioSettings)
        )
        Button(
            label = tr("Overlay settings"),
            onClick = dropUnlessResumed(block = goToOverlaySettings)
        )
        Button(
            label = tr("Emulated USB Devices"),
            onClick = dropUnlessResumed(block = goToEmulatedUSBDevicesSettings)
        )
        Button(
            label = tr("Account settings"),
            onClick = dropUnlessResumed(block = goToAccountSettings)
        )
        Button(
            label = if (validationEnabled) "Vulkan validation: ON (slower)" else "Enable Vulkan validation (slower)",
            onClick = dropUnlessResumed {
                validationEnabled = !validationEnabled
                NativeEmulation.setVulkanValidationEnabled(validationEnabled)
                Toast.makeText(
                    context,
                    if (validationEnabled) "Vulkan validation enabled for the next game launch" else "Vulkan validation disabled",
                    Toast.LENGTH_LONG
                ).show()
            }
        )
        Button(
            label = "Export Vulkan diagnostic log (.txt)",
            onClick = dropUnlessResumed {
                createLogDocument.launch("cemu-vulkan-diagnostic.txt")
            }
        )
    }
}