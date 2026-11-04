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
    var monitorEnabled by remember { mutableStateOf(NativeEmulation.isMonitorEnabled()) }
    val createLogDocument = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("text/plain")
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                try {
                    val report = buildString {
                        append(NativeEmulation.getMonitorLog())
                        append("\n\nAndroid device: ${Build.MANUFACTURER} ${Build.MODEL}")
                        append("\nAndroid API level: ${Build.VERSION.SDK_INT}\n")
                    }
                    val output = context.contentResolver.openOutputStream(uri)
                        ?: throw IllegalStateException("Unable to open the selected destination")
                    output.use { it.write(report.toByteArray(Charsets.UTF_8)) }
                    withContext(Dispatchers.Main) {
                        Toast.makeText(context, "Relatório do Monitor 2.0 exportado", Toast.LENGTH_LONG).show()
                    }
                } catch (exception: Exception) {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(context, "Falha ao exportar o relatório do Monitor 2.0: ${exception.message ?: "erro desconhecido"}", Toast.LENGTH_LONG).show()
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
            label = if (monitorEnabled) "Monitor 2.0: ATIVO" else "Ativar Monitor 2.0",
            onClick = dropUnlessResumed {
                monitorEnabled = !monitorEnabled
                NativeEmulation.setMonitorEnabled(monitorEnabled)
                Toast.makeText(
                    context,
                    if (monitorEnabled) "Monitor 2.0 ativado" else "Monitor 2.0 desativado",
                    Toast.LENGTH_LONG
                ).show()
            }
        )
        Button(
            label = "Exportar Monitor 2.0 (.txt)",
            onClick = dropUnlessResumed {
                createLogDocument.launch("cemu-monitor-2.0.txt")
            }
        )
    }
}