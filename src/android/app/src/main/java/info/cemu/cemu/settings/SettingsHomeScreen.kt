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
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.dropUnlessResumed
import info.cemu.cemu.R
import info.cemu.cemu.guicore.components.Button
import info.cemu.cemu.guicore.components.ScreenContent

data class SettingsHomeScreenActions(
    val goToGeneralSettings: () -> Unit,
    val goToInputSettings: () -> Unit,
    val goToGraphicsSettings: () -> Unit,
    val goToAudioSettings: () -> Unit,
    val goToOverlaySettings: () -> Unit,
)

@Composable

fun SettingsHomeScreen(navigateBack: () -> Unit, actions: SettingsHomeScreenActions) {
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
                        Toast.makeText(context, "Failed to export Vulkan log: ${exception.message ?: "unknown error"}", Toast.LENGTH_LONG).show()
                    }
                }
            }
        }
    }
    ScreenContent(
        appBarText = stringResource(R.string.settings),
        navigateBack = navigateBack,
    ) {
        Button(
            label = stringResource(R.string.general_settings),
            onClick = dropUnlessResumed(block = actions.goToGeneralSettings)
        )
        Button(
            label = stringResource(R.string.input_settings),
            onClick = dropUnlessResumed(block = actions.goToInputSettings)
        )
        Button(
            label = stringResource(R.string.graphics_settings),
            onClick = dropUnlessResumed(block = actions.goToGraphicsSettings)
        )
        Button(
            label = stringResource(R.string.audio_settings),
            onClick = dropUnlessResumed(block = actions.goToAudioSettings)
        )
        Button(
            label = stringResource(R.string.overlay_settings),
            onClick = dropUnlessResumed(block = actions.goToOverlaySettings)
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