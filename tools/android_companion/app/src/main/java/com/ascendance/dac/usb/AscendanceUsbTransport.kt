package com.ascendance.dac.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

data class UsbConnectionInfo(
    val connected: Boolean = false,
    val deviceName: String = "Disconnected",
    val productName: String = "",
    val vendorId: Int = 0,
    val productId: Int = 0,
    val hidInterfaceId: Int = -1,
    val epOutAddress: Int = -1,
    val epInAddress: Int = -1
)

class AscendanceUsbTransport(context: Context) {

    private val appContext = context.applicationContext
    private val usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val sendMutex = Mutex()
    private val writeChannel = Channel<ByteArray>(capacity = 64)

    private var connection: UsbDeviceConnection? = null
    private var claimedInterface: UsbInterface? = null
    private var epOut: UsbEndpoint? = null
    private var epIn: UsbEndpoint? = null
    private var activeDevice: UsbDevice? = null

    private var readerJob: Job? = null
    private var writerJob: Job? = null

    private val _connectionInfo = MutableStateFlow(UsbConnectionInfo())
    val connectionInfo: StateFlow<UsbConnectionInfo> = _connectionInfo.asStateFlow()

    private val _incomingMessages = MutableSharedFlow<HidIncomingMessage>(extraBufferCapacity = 64)
    val incomingMessages: SharedFlow<HidIncomingMessage> = _incomingMessages.asSharedFlow()

    private val _logs = MutableSharedFlow<String>(extraBufferCapacity = 64)
    val logs: SharedFlow<String> = _logs.asSharedFlow()

    private val _permissionEvents = MutableSharedFlow<Boolean>(extraBufferCapacity = 4)
    val permissionEvents: SharedFlow<Boolean> = _permissionEvents.asSharedFlow()

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device: UsbDevice? = getUsbDeviceExtra(intent)
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    if (granted && device != null) {
                        log("USB HID permission granted for ${device.productName ?: device.deviceName}.")
                        openDevice(device)
                    } else {
                        log("USB HID permission denied by user.")
                        _permissionEvents.tryEmit(false)
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    val device: UsbDevice? = getUsbDeviceExtra(intent)
                    if (device != null && isMatchingDac(device)) {
                        log("Detected attached ASCENDANCE DAC (${device.productName ?: "USB Audio"}). Connecting...")
                        connectOrRequestPermission(device)
                    }
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val device: UsbDevice? = getUsbDeviceExtra(intent)
                    if (device != null && activeDevice?.deviceId == device.deviceId) {
                        log("USB DAC detached.")
                        disconnect()
                    }
                }
            }
        }
    }

    init {
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        ContextCompat.registerReceiver(appContext, usbReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    fun handleNewIntent(intent: Intent?) {
        if (intent?.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            val dev = getUsbDeviceExtra(intent)
            if (dev != null && isMatchingDac(dev)) {
                connectOrRequestPermission(dev)
            }
        }
    }

    fun scanAndConnect() {
        val candidate = usbManager.deviceList.values.firstOrNull { isMatchingDac(it) }
        if (candidate == null) {
            log("No ASCENDANCE DAC found on USB.")
            return
        }
        connectOrRequestPermission(candidate)
    }

    fun connectOrRequestPermission(device: UsbDevice) {
        if (activeDevice?.deviceId == device.deviceId && _connectionInfo.value.connected) {
            return
        }
        if (usbManager.hasPermission(device)) {
            openDevice(device)
        } else {
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            } else {
                PendingIntent.FLAG_UPDATE_CURRENT
            }
            val permIntent = Intent(ACTION_USB_PERMISSION).apply {
                setPackage(appContext.packageName)
            }
            val pendingIntent = PendingIntent.getBroadcast(appContext, 0, permIntent, flags)
            log("Requesting Android USB permission for ${device.productName ?: "USB DAC"}...")
            usbManager.requestPermission(device, pendingIntent)
        }
    }

    private fun openDevice(device: UsbDevice) {
        disconnect()

        val hidIntf = findHidInterface(device)
        if (hidIntf == null) {
            log("Error: Device has no USB control interface.")
            return
        }

        var foundOut: UsbEndpoint? = null
        var foundIn: UsbEndpoint? = null
        for (i in 0 until hidIntf.endpointCount) {
            val ep = hidIntf.getEndpoint(i)
            if (ep.direction == UsbConstants.USB_DIR_OUT) foundOut = ep
            if (ep.direction == UsbConstants.USB_DIR_IN) foundIn = ep
        }

        val conn = usbManager.openDevice(device)
        if (conn == null) {
            log("Error: Failed to open USB device.")
            return
        }

        // Claim ONLY Interface 2 (HID) so Android kernel snd-usb-audio continues playing UAC 1.0 audio.
        if (!conn.claimInterface(hidIntf, true)) {
            log("Error: Failed to claim control interface #${hidIntf.id}.")
            conn.close()
            return
        }

        activeDevice = device
        connection = conn
        claimedInterface = hidIntf
        epOut = foundOut
        epIn = foundIn

        _connectionInfo.value = UsbConnectionInfo(
            connected = true,
            deviceName = device.deviceName,
            productName = device.productName ?: "ASCENDANCE Hi-Fi DAC",
            vendorId = device.vendorId,
            productId = device.productId,
            hidInterfaceId = hidIntf.id,
            epOutAddress = foundOut?.address ?: -1,
            epInAddress = foundIn?.address ?: -1
        )
        _permissionEvents.tryEmit(true)
        log("Connected HID interface #${hidIntf.id} (UAC Audio untouched). EP OUT=${foundOut?.address ?: -1}, EP IN=${foundIn?.address ?: -1}")

        startIoLoops()
    }

    private fun startIoLoops() {
        writerJob?.cancel()
        readerJob?.cancel()

        writerJob = scope.launch {
            while (isActive) {
                val packet = writeChannel.receive()
                sendNow(packet)
            }
        }

        val inEndpoint = epIn ?: return
        readerJob = scope.launch {
            val buffer = ByteArray(PeqPacketCodec.REPORT_SIZE)
            while (isActive && _connectionInfo.value.connected) {
                val conn = connection ?: break
                val read = conn.bulkTransfer(inEndpoint, buffer, buffer.size, 250)
                if (read > 0) {
                    val copy = buffer.copyOf(read)
                    PeqPacketCodec.parseReport(copy)?.let { msg ->
                        _incomingMessages.tryEmit(msg)
                    }
                } else if (read < 0) {
                    // Timeout (-1) is normal when DAC has no unsolicited HID report; brief yield
                    delay(5)
                }
            }
        }
    }

    fun sendPacket(packet: ByteArray) {
        if (!_connectionInfo.value.connected) return
        if (writeChannel.trySend(packet).isFailure) {
            scope.launch { writeChannel.send(packet) }
        }
    }

    private suspend fun sendNow(packet: ByteArray) {
        sendMutex.withLock {
            val conn = connection ?: return
            val out = epOut
            val intf = claimedInterface
            val sent = if (out != null) {
                conn.bulkTransfer(out, packet, packet.size, 100)
            } else {
                -1
            }
            if (sent < 0 && intf != null) {
                // Fallback to USB HID SET_REPORT control transfer (bmRequestType=0x21, bRequest=0x09)
                val ctrl = conn.controlTransfer(
                    0x21,
                    0x09,
                    0x0200,
                    intf.id,
                    packet,
                    packet.size,
                    150
                )
                if (ctrl < 0) {
                    log("HID write error (bulk=$sent, ctrl=$ctrl).")
                }
            }
        }
    }

    fun disconnect() {
        readerJob?.cancel()
        writerJob?.cancel()
        readerJob = null
        writerJob = null

        val conn = connection
        val intf = claimedInterface
        connection = null
        claimedInterface = null
        epOut = null
        epIn = null
        activeDevice = null

        if (conn != null) {
            try {
                if (intf != null) conn.releaseInterface(intf)
                conn.close()
            } catch (_: Throwable) {
            }
        }
        _connectionInfo.value = UsbConnectionInfo()
    }

    fun release() {
        disconnect()
        try {
            appContext.unregisterReceiver(usbReceiver)
        } catch (_: Throwable) {
        }
    }

    private fun isMatchingDac(device: UsbDevice): Boolean {
        if (device.vendorId == PeqPacketCodec.USB_VENDOR_ID &&
            device.productId == PeqPacketCodec.USB_PRODUCT_ID
        ) {
            return true
        }
        return findHidInterface(device) != null &&
            (device.productName?.contains("ASCENDANCE", ignoreCase = true) == true ||
                device.productName?.contains("ESP32-S3", ignoreCase = true) == true)
    }

    private fun findHidInterface(device: UsbDevice): UsbInterface? {
        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            if (intf.interfaceClass == UsbConstants.USB_CLASS_HID) {
                return intf
            }
        }
        return null
    }

    @Suppress("DEPRECATION")
    private fun getUsbDeviceExtra(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    private fun log(msg: String) {
        _logs.tryEmit(msg)
    }

    companion object {
        private const val ACTION_USB_PERMISSION = "com.ascendance.dac.USB_PERMISSION"
    }
}
