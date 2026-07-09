package com.example.fm.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeveloperMode
import androidx.compose.material.icons.filled.FastForward
import androidx.compose.material.icons.filled.FastRewind
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.Headset
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.Radio
import androidx.compose.material.icons.filled.SignalCellularAlt
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.VolumeMute
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Paint
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.fm.data.FmPreset

@Composable
fun FmScreen(
    viewModel: FmViewModel,
    modifier: Modifier = Modifier
) {
    val isPowerOn by viewModel.isPowerOn.collectAsState()
    val currentFreqKHz by viewModel.currentFreqKHz.collectAsState()
    val isMuted by viewModel.isMuted.collectAsState()
    val rssi by viewModel.rssi.collectAsState()
    val rdsText by viewModel.rdsText.collectAsState()
    val isScanning by viewModel.isScanning.collectAsState()
    val isHwSupported by viewModel.isHwSupported.collectAsState()
    val audioRoute by viewModel.audioRoute.collectAsState()
    val headsetConnected by viewModel.headsetConnected.collectAsState()
    val savedPresets by viewModel.savedPresets.collectAsState()

    var showArchitectureDetails by remember { mutableStateOf(false) }

    Scaffold(
        modifier = modifier.fillMaxSize(),
        topBar = {
            FmTopBar(isHwSupported = isHwSupported)
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .padding(innerPadding)
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // Warning if headphones are missing (Headphones act as physical FM aerial antenna)
            AnimatedVisibility(visible = !headsetConnected) {
                AntennaWarningCard()
            }

            // Central Dashboard
            FmDashboard(
                isPowerOn = isPowerOn,
                currentFreqKHz = currentFreqKHz,
                rssi = rssi,
                rdsText = rdsText,
                isScanning = isScanning
            )

            // Dynamic Tuning Dial Slider (Compose Canvas implementation)
            TuningDial(
                currentFreqKHz = currentFreqKHz,
                onFreqChanged = { freq ->
                    if (isPowerOn) {
                        viewModel.setFrequency(freq)
                    }
                },
                modifier = Modifier.testTag("tuning_dial_canvas")
            )

            // Tuner Button Controls
            TunerControlPanel(
                viewModel = viewModel,
                isPowerOn = isPowerOn,
                isMuted = isMuted,
                currentFreqKHz = currentFreqKHz,
                savedPresets = savedPresets,
                audioRoute = audioRoute
            )

            // Saved Station Presets
            PresetsSection(
                presets = savedPresets,
                currentFreqKHz = currentFreqKHz,
                onSelectPreset = { preset ->
                    if (isPowerOn) {
                        viewModel.selectPreset(preset)
                    }
                }
            )

            // Architectural Blueprint Visualizer (Mandate from architectural blueprint requirements)
            ArchitectureBlueprintCard(
                expanded = showArchitectureDetails,
                onToggleExpand = { showArchitectureDetails = !showArchitectureDetails }
            )
        }
    }
}

@Composable
fun FmTopBar(isHwSupported: Boolean) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .statusBarsPadding()
            .background(MaterialTheme.colorScheme.surface)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                imageVector = Icons.Default.Radio,
                contentDescription = "Radio Icon",
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp)
            )
            Spacer(modifier = Modifier.width(10.dp))
            Text(
                text = "Qualcomm FM Radio",
                style = MaterialTheme.typography.titleLarge.copy(
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 0.5.sp
                ),
                color = MaterialTheme.colorScheme.onSurface
            )
        }

        // Hardware Status Chip
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(12.dp))
                .background(
                    if (isHwSupported) Color(0xFF1B5E20).copy(alpha = 0.15f)
                    else Color(0xFFB71C1C).copy(alpha = 0.15f)
                )
                .padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(if (isHwSupported) Color.Green else Color.Red)
            )
            Spacer(modifier = Modifier.width(6.dp))
            Text(
                text = if (isHwSupported) "HAL ACTIVE" else "NO HW DETECTED",
                style = MaterialTheme.typography.labelSmall.copy(fontWeight = FontWeight.Bold),
                color = if (isHwSupported) Color(0xFF2E7D32) else Color(0xFFC62828)
            )
        }
    }
}

@Composable
fun AntennaWarningCard() {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer
        ),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Default.Warning,
                contentDescription = "Warning",
                tint = MaterialTheme.colorScheme.onErrorContainer,
                modifier = Modifier.size(24.dp)
            )
            Spacer(modifier = Modifier.width(12.dp))
            Column {
                Text(
                    text = "No Antenna Detected",
                    style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Bold),
                    color = MaterialTheme.colorScheme.onErrorContainer
                )
                Text(
                    text = "Please connect wired headphones. The headphone wire serves as the FM aerial antenna.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onErrorContainer.copy(alpha = 0.85f)
                )
            }
        }
    }
}

@Composable
fun FmDashboard(
    isPowerOn: Boolean,
    currentFreqKHz: Int,
    rssi: Int,
    rdsText: String?,
    isScanning: Boolean
) {
    val infiniteTransition = rememberInfiniteTransition(label = "scanning_pulse")
    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.3f,
        targetValue = 1.0f,
        animationSpec = infiniteRepeatable(
            animation = tween(800),
            repeatMode = RepeatMode.Reverse
        ),
        label = "pulse"
    )

    Card(
        colors = CardDefaults.cardColors(
            containerColor = if (isPowerOn) MaterialTheme.colorScheme.surfaceVariant
            else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f)
        ),
        shape = RoundedCornerShape(16.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Signal and RDS line
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Signal Indicator
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.SignalCellularAlt,
                        contentDescription = "Signal Strength",
                        tint = if (isPowerOn) MaterialTheme.colorScheme.primary else Color.Gray,
                        modifier = Modifier.size(20.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = if (isPowerOn) "RSSI: $rssi dBm" else "RSSI: --",
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (isPowerOn) MaterialTheme.colorScheme.onSurfaceVariant
                        else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                    )
                }

                // Stereo / Mono Indicator
                Text(
                    text = if (isPowerOn) "FM STEREO" else "OFFLINE",
                    style = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.Bold),
                    color = if (isPowerOn) MaterialTheme.colorScheme.primary else Color.Gray
                )
            }

            // Big Digital Tuner Readout
            val displayFreq = currentFreqKHz / 1000f
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.padding(vertical = 8.dp)
            ) {
                Text(
                    text = if (isPowerOn) String.format("%.1f", displayFreq) else "87.5",
                    fontSize = 64.sp,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.ExtraBold,
                    color = if (isPowerOn) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f),
                    modifier = Modifier.alpha(if (isScanning) pulseAlpha else 1.0f)
                )
                Text(
                    text = "MHz",
                    style = MaterialTheme.typography.titleMedium.copy(
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 2.sp
                    ),
                    color = if (isPowerOn) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f)
                )
            }

            HorizontalDivider(
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.1f)
            )

            // Radio RDS/RT Info Text
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
                verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                if (isPowerOn) {
                    if (isScanning) {
                        Text(
                            text = "SCANNING FM RECEIVER...",
                            style = MaterialTheme.typography.bodyMedium.copy(
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.primary
                            ),
                            modifier = Modifier.alpha(pulseAlpha)
                        )
                    } else {
                        Text(
                            text = rdsText ?: "NO RDS DATA AVAILABLE",
                            style = MaterialTheme.typography.bodyMedium.copy(
                                fontWeight = FontWeight.SemiBold,
                                color = if (rdsText != null) MaterialTheme.colorScheme.onSurfaceVariant
                                else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                            )
                        )
                    }
                } else {
                    Text(
                        text = "POWER OFF",
                        style = MaterialTheme.typography.bodyMedium.copy(
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
                        )
                    )
                }
            }
        }
    }
}

@Composable
fun TuningDial(
    currentFreqKHz: Int,
    onFreqChanged: (Int) -> Unit,
    modifier: Modifier = Modifier
) {
    val minFreq = 87500
    val maxFreq = 108000

    BoxWithConstraints(
        modifier = modifier
            .fillMaxWidth()
            .height(120.dp)
            .background(
                brush = Brush.verticalGradient(
                    colors = listOf(
                        MaterialTheme.colorScheme.surfaceVariant,
                        MaterialTheme.colorScheme.surface
                    )
                ),
                shape = RoundedCornerShape(16.dp)
            )
            .border(1.5.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(16.dp))
            .pointerInput(Unit) {
                detectDragGestures { change, dragAmount ->
                    change.consume()
                    // dragAmount.x drag speed multiplier
                    val khzPerPx = 20f
                    val delta = -(dragAmount.x * khzPerPx).toInt()
                    val target = (currentFreqKHz + delta).coerceIn(minFreq, maxFreq)
                    // Snap to standard 100KHz raster
                    val snapped = ((target + 50) / 100) * 100
                    onFreqChanged(snapped)
                }
            }
    ) {
        val widthPx = constraints.maxWidth.toFloat()
        val middlePx = widthPx / 2f
        val lineTextColor = MaterialTheme.colorScheme.onSurface.toArgb()
        val tickColorSecondary = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f).toArgb()

        Canvas(modifier = Modifier.fillMaxSize()) {
            // Screen covers 12000 KHz (12 MHz) around current tuned frequency
            val pixelsPerKHz = widthPx / 12000f

            for (f in minFreq..maxFreq step 100) {
                val khzDiff = f - currentFreqKHz
                val x = middlePx + (khzDiff * pixelsPerKHz)

                if (x in 0f..widthPx) {
                    val isMajor = f % 1000 == 0
                    val isHalf = f % 500 == 0

                    val tickHeight = when {
                        isMajor -> 40f
                        isHalf -> 25f
                        else -> 15f
                    }
                    val col = if (isMajor) lineTextColor else tickColorSecondary
                    val sw = if (isMajor) 4f else 2f

                    drawLine(
                        color = Color(col),
                        start = Offset(x, size.height - tickHeight - 15f),
                        end = Offset(x, size.height - 15f),
                        strokeWidth = sw
                    )

                    if (isMajor) {
                        val mhzLabel = (f / 1000).toString()
                        drawContext.canvas.nativeCanvas.apply {
                            val paint = android.graphics.Paint().apply {
                                color = lineTextColor
                                textSize = 30f
                                textAlign = android.graphics.Paint.Align.CENTER
                                isFakeBoldText = true
                            }
                            drawText(mhzLabel, x, size.height - tickHeight - 30f, paint)
                        }
                    }
                }
            }

            // Draw Red Center Tuner Marker line
            drawLine(
                color = Color.Red,
                start = Offset(middlePx, 10f),
                end = Offset(middlePx, size.height - 10f),
                strokeWidth = 6f
            )
            drawCircle(
                color = Color.Red,
                center = Offset(middlePx, size.height - 10f),
                radius = 8f
            )
        }
    }
}

@Composable
fun TunerControlPanel(
    viewModel: FmViewModel,
    isPowerOn: Boolean,
    isMuted: Boolean,
    currentFreqKHz: Int,
    savedPresets: List<FmPreset>,
    audioRoute: String
) {
    val hasFavorite = savedPresets.any { it.frequencyKHz == currentFreqKHz }
    val powerButtonColor by animateColorAsState(
        targetValue = if (isPowerOn) Color(0xFF2E7D32) else MaterialTheme.colorScheme.secondary,
        label = "power_color"
    )

    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // Power Row
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Button(
                onClick = { viewModel.togglePower() },
                colors = ButtonDefaults.buttonColors(containerColor = powerButtonColor),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier
                    .weight(1.2f)
                    .height(56.dp)
                    .testTag("power_button")
            ) {
                Icon(
                    imageVector = Icons.Default.PowerSettingsNew,
                    contentDescription = "Power Toggle"
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = if (isPowerOn) "FM ON" else "FM OFF",
                    style = MaterialTheme.typography.titleMedium.copy(fontWeight = FontWeight.Bold)
                )
            }

            // Audio Routing Control
            Button(
                onClick = { viewModel.toggleAudioRoute() },
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.secondaryContainer,
                    contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                ),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier
                    .weight(1.0f)
                    .height(56.dp)
                    .testTag("audio_route_button")
            ) {
                Icon(
                    imageVector = Icons.Default.Headset,
                    contentDescription = "Audio Route"
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = audioRoute,
                    style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Bold)
                )
            }
        }

        // Tuning & Scanning Row
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Seek Backwards
            IconButton(
                onClick = { viewModel.startScan(false) },
                enabled = isPowerOn,
                modifier = Modifier
                    .size(52.dp)
                    .clip(CircleShape)
                    .background(
                        if (isPowerOn) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
                    )
                    .testTag("seek_down_button")
            ) {
                Icon(
                    imageVector = Icons.Default.FastRewind,
                    contentDescription = "Seek Down",
                    tint = if (isPowerOn) MaterialTheme.colorScheme.onPrimaryContainer else Color.Gray
                )
            }

            // Step Down 100KHz
            IconButton(
                onClick = { viewModel.tuneStep(false) },
                enabled = isPowerOn,
                modifier = Modifier
                    .size(52.dp)
                    .clip(CircleShape)
                    .background(
                        if (isPowerOn) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
                    )
                    .testTag("step_down_button")
            ) {
                Icon(
                    imageVector = Icons.Default.SkipPrevious,
                    contentDescription = "Step Down",
                    tint = if (isPowerOn) MaterialTheme.colorScheme.onPrimaryContainer else Color.Gray
                )
            }

            // Save Favorite Button
            IconButton(
                onClick = { viewModel.toggleFavorite() },
                enabled = isPowerOn,
                modifier = Modifier
                    .size(64.dp)
                    .clip(CircleShape)
                    .background(
                        if (isPowerOn) {
                            if (hasFavorite) Color(0xFFE57373) else MaterialTheme.colorScheme.primaryContainer
                        } else {
                            MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
                        }
                    )
                    .testTag("favorite_toggle_button")
            ) {
                Icon(
                    imageVector = if (hasFavorite) Icons.Default.Favorite else Icons.Default.FavoriteBorder,
                    contentDescription = "Save Station Favorite",
                    tint = if (isPowerOn) {
                        if (hasFavorite) Color.White else MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        Color.Gray
                    },
                    modifier = Modifier.size(28.dp)
                )
            }

            // Step Up 100KHz
            IconButton(
                onClick = { viewModel.tuneStep(true) },
                enabled = isPowerOn,
                modifier = Modifier
                    .size(52.dp)
                    .clip(CircleShape)
                    .background(
                        if (isPowerOn) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
                    )
                    .testTag("step_up_button")
            ) {
                Icon(
                    imageVector = Icons.Default.SkipNext,
                    contentDescription = "Step Up",
                    tint = if (isPowerOn) MaterialTheme.colorScheme.onPrimaryContainer else Color.Gray
                )
            }

            // Seek Upwards
            IconButton(
                onClick = { viewModel.startScan(true) },
                enabled = isPowerOn,
                modifier = Modifier
                    .size(52.dp)
                    .clip(CircleShape)
                    .background(
                        if (isPowerOn) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
                    )
                    .testTag("seek_up_button")
            ) {
                Icon(
                    imageVector = Icons.Default.FastForward,
                    contentDescription = "Seek Up",
                    tint = if (isPowerOn) MaterialTheme.colorScheme.onPrimaryContainer else Color.Gray
                )
            }
        }

        // Mute Row
        Button(
            onClick = { viewModel.toggleMute() },
            enabled = isPowerOn,
            colors = ButtonDefaults.buttonColors(
                containerColor = if (isMuted) Color(0xFFC62828) else MaterialTheme.colorScheme.surfaceVariant,
                contentColor = if (isMuted) Color.White else MaterialTheme.colorScheme.onSurfaceVariant
            ),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier
                .fillMaxWidth()
                .height(48.dp)
                .testTag("mute_button")
        ) {
            Icon(
                imageVector = if (isMuted) Icons.Default.VolumeMute else Icons.Default.VolumeUp,
                contentDescription = "Mute Toggle"
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = if (isMuted) "UNMUTE FM AUDIO" else "MUTE FM AUDIO",
                style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Bold)
            )
        }
    }
}

@Composable
fun PresetsSection(
    presets: List<FmPreset>,
    currentFreqKHz: Int,
    onSelectPreset: (FmPreset) -> Unit
) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = "Station Presets (${presets.size})",
            style = MaterialTheme.typography.titleMedium.copy(fontWeight = FontWeight.Bold),
            color = MaterialTheme.colorScheme.onBackground
        )

        if (presets.isEmpty()) {
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(64.dp)
            ) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "No saved presets yet. Tune and click heart to save.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                    )
                }
            }
        } else {
            LazyRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                contentPadding = PaddingValues(vertical = 4.dp)
            ) {
                items(presets) { preset ->
                    val isTuned = preset.frequencyKHz == currentFreqKHz
                    val cardBorder = if (isTuned) {
                        Modifier.border(2.dp, MaterialTheme.colorScheme.primary, RoundedCornerShape(12.dp))
                    } else {
                        Modifier
                    }

                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = if (isTuned) MaterialTheme.colorScheme.primaryContainer
                            else MaterialTheme.colorScheme.surface
                        ),
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier
                            .width(130.dp)
                            .then(cardBorder)
                            .clickable { onSelectPreset(preset) }
                            .testTag("preset_card_${preset.frequencyKHz}")
                    ) {
                        Column(
                            modifier = Modifier.padding(12.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp)
                        ) {
                            Text(
                                text = "${preset.frequencyMhz} MHz",
                                style = MaterialTheme.typography.bodyLarge.copy(
                                    fontWeight = FontWeight.Bold,
                                    fontFamily = FontFamily.Monospace
                                ),
                                color = if (isTuned) MaterialTheme.colorScheme.onPrimaryContainer
                                else MaterialTheme.colorScheme.onSurface
                            )
                            Text(
                                text = preset.stationName,
                                style = MaterialTheme.typography.labelSmall,
                                maxLines = 1,
                                color = if (isTuned) MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.8f)
                                else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun ArchitectureBlueprintCard(
    expanded: Boolean,
    onToggleExpand: () -> Unit
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f)
        ),
        shape = RoundedCornerShape(16.dp),
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onToggleExpand() }
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.DeveloperMode,
                        contentDescription = "Architecture Blueprint",
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(24.dp)
                    )
                    Spacer(modifier = Modifier.width(10.dp))
                    Text(
                        text = "Qualcomm FM HAL Blueprint",
                        style = MaterialTheme.typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Icon(
                    imageVector = if (expanded) Icons.Default.KeyboardArrowUp else Icons.Default.KeyboardArrowDown,
                    contentDescription = "Expand",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            if (expanded) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Text(
                        text = "This application communicates directly with the physical Qualcomm FM hardware via the official Android Hardware Abstraction Layer (HAL) architecture stack:",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.85f)
                    )

                    // Architecture Stack visual block
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(
                                MaterialTheme.colorScheme.surface,
                                RoundedCornerShape(8.dp)
                            )
                            .padding(12.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        StackLayerWidget("Application Layer (Kotlin / Compose)", "Active UI Controllers & Preset Database")
                        StackArrow()
                        StackLayerWidget("JNI Layer (C++)", "Dynamic Binder Resolver & dlopen() Connector")
                        StackArrow()
                        StackLayerWidget("HAL Service (vendor.qti.hardware.fm@1.0.so)", "Official Qualcomm FM Binder Definition")
                        StackArrow()
                        StackLayerWidget("HAL Impl (vendor.qti.hardware.fm@1.0-impl.so)", "Hardware Abstraction Implementation")
                        StackArrow()
                        StackLayerWidget("Radio PAL (libfmpal.so / V4L2 Driver)", "Low-Level Radio Tuner driver / /dev/radio0")
                    }

                    Text(
                        text = "Reliability Guard: If physical Qualcomm FM hardware libraries are missing (such as inside simulated environments or non-Qualcomm reference devices), the dynamic JNI resolver detects and bypasses HAL lookups gracefully to avoid driver crashes, falling back to clean offline information banners.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                    )
                }
            }
        }
    }
}

@Composable
fun StackLayerWidget(title: String, subtitle: String) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(6.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f))
            .padding(8.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.Bold),
            color = MaterialTheme.colorScheme.primary
        )
        Text(
            text = subtitle,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
        )
    }
}

@Composable
fun StackArrow() {
    Text(
        text = "↓",
        fontSize = 18.sp,
        fontWeight = FontWeight.Bold,
        color = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f),
        modifier = Modifier.padding(vertical = 2.dp)
    )
}
