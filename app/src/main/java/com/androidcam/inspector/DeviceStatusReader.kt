package com.androidcam.inspector

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.PowerManager

data class DeviceStatus(
    val batteryPercent: Int?,
    val batteryState: String,
    val powerSource: String,
    val batteryHealth: String,
    val batteryTemperatureC: Float?,
    val currentNowMilliamps: Float?,
    val currentAverageMilliamps: Float?,
    val voltageVolts: Float?,
    val approximatePowerWatts: Float?,
    val thermalStatus: String
)

class DeviceStatusReader(private val context: Context) {
    fun read(): DeviceStatus {
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val percent = if (level >= 0 && scale > 0) (level * 100f / scale).toInt().coerceIn(0, 100) else null
        val status = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN)
            ?: BatteryManager.BATTERY_STATUS_UNKNOWN
        val plugged = battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        val health = battery?.getIntExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_UNKNOWN)
            ?: BatteryManager.BATTERY_HEALTH_UNKNOWN
        val temperatureTenths = battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            ?: Int.MIN_VALUE
        val voltageMillivolts = battery?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, Int.MIN_VALUE)
            ?: Int.MIN_VALUE
        val batteryManager = context.getSystemService(BatteryManager::class.java)
        fun currentMilliamps(property: Int): Float? {
            val microamps = batteryManager.getIntProperty(property)
            return microamps.takeIf { it != Int.MIN_VALUE && kotlin.math.abs(it.toLong()) <= 20_000_000L }
                ?.div(1000f)
        }
        val currentNowMilliamps = currentMilliamps(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
        val currentAverageMilliamps = currentMilliamps(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE)
        val voltageVolts = voltageMillivolts.takeIf { it in 1..30_000 }?.div(1000f)
        val approximatePowerWatts = if (currentNowMilliamps != null && voltageVolts != null)
            kotlin.math.abs(currentNowMilliamps) * voltageVolts / 1000f
        else null
        val thermal = context.getSystemService(PowerManager::class.java).currentThermalStatus
        return DeviceStatus(
            batteryPercent = percent,
            batteryState = when (status) {
                BatteryManager.BATTERY_STATUS_CHARGING -> "charging"
                BatteryManager.BATTERY_STATUS_DISCHARGING -> "discharging"
                BatteryManager.BATTERY_STATUS_FULL -> "full"
                BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "not_charging"
                else -> "unknown"
            },
            powerSource = when {
                plugged and BatteryManager.BATTERY_PLUGGED_USB != 0 -> "usb"
                plugged and BatteryManager.BATTERY_PLUGGED_AC != 0 -> "ac"
                plugged and BatteryManager.BATTERY_PLUGGED_WIRELESS != 0 -> "wireless"
                else -> "unknown"
            },
            batteryHealth = when (health) {
                BatteryManager.BATTERY_HEALTH_GOOD -> "good"
                BatteryManager.BATTERY_HEALTH_OVERHEAT -> "overheat"
                BatteryManager.BATTERY_HEALTH_DEAD -> "dead"
                BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE -> "over_voltage"
                BatteryManager.BATTERY_HEALTH_UNSPECIFIED_FAILURE -> "failure"
                BatteryManager.BATTERY_HEALTH_COLD -> "cold"
                else -> "unknown"
            },
            batteryTemperatureC = temperatureTenths.takeIf { it != Int.MIN_VALUE }?.div(10f),
            currentNowMilliamps = currentNowMilliamps,
            currentAverageMilliamps = currentAverageMilliamps,
            voltageVolts = voltageVolts,
            approximatePowerWatts = approximatePowerWatts,
            thermalStatus = when (thermal) {
                PowerManager.THERMAL_STATUS_NONE -> "none"
                PowerManager.THERMAL_STATUS_LIGHT -> "light"
                PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
                PowerManager.THERMAL_STATUS_SEVERE -> "severe"
                PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
                PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
                PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
                else -> "unknown"
            }
        )
    }
}
