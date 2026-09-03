package ru.solar.setup

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.util.ArrayDeque
import java.util.UUID

/**
 * Настройка сети модуля мониторинга по BLE.
 *
 * Сервис и характеристики совпадают с прошивкой (src/prov.cpp):
 *   9a1f0001-…  сервис
 *   0002 W      имя сети
 *   0003 W      пароль
 *   0004 W      команда: 1 — искать сети, 2 — сохранить, 3 — сброс
 *   0005 RN     статус и результаты поиска
 */
class MainActivity : AppCompatActivity() {

    companion object {
        val SVC:  UUID = UUID.fromString("9a1f0001-7f3a-4b0e-9f2b-1c7a5d3e0001")
        val SSID: UUID = UUID.fromString("9a1f0002-7f3a-4b0e-9f2b-1c7a5d3e0001")
        val PASS: UUID = UUID.fromString("9a1f0003-7f3a-4b0e-9f2b-1c7a5d3e0001")
        val CTRL: UUID = UUID.fromString("9a1f0004-7f3a-4b0e-9f2b-1c7a5d3e0001")
        val STAT: UUID = UUID.fromString("9a1f0005-7f3a-4b0e-9f2b-1c7a5d3e0001")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val CMD_SCAN = 1.toByte()
        const val CMD_APPLY = 2.toByte()
    }

    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var deviceList: LinearLayout
    private lateinit var netList: LinearLayout
    private lateinit var wifiBlock: View
    private lateinit var ssidEdit: EditText
    private lateinit var passEdit: EditText

    private val ui = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var matchByName = false
    private val seen = HashMap<String, BluetoothDevice>()

    /** BLE допускает одну операцию за раз — держим очередь. */
    private val queue = ArrayDeque<() -> Unit>()
    private var busy = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        status = findViewById(R.id.status)
        log = findViewById(R.id.log)
        deviceList = findViewById(R.id.deviceList)
        netList = findViewById(R.id.netList)
        wifiBlock = findViewById(R.id.wifiBlock)
        ssidEdit = findViewById(R.id.ssid)
        passEdit = findViewById(R.id.pass)

        findViewById<Button>(R.id.btnScan).setOnClickListener { ensurePermissions { startScan() } }
        findViewById<Button>(R.id.btnWifiScan).setOnClickListener {
            netList.removeAllViews()
            enqueue { writeCtrl(CMD_SCAN) }
        }
        findViewById<Button>(R.id.btnSave).setOnClickListener { saveCredentials() }
    }

    override fun onDestroy() {
        super.onDestroy()
        closeGatt()
    }

    // ------------------------------------------------------------ разрешения

    private var pendingAction: (() -> Unit)? = null

    private fun neededPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun ensurePermissions(action: () -> Unit) {
        val missing = neededPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) { action(); return }
        pendingAction = action
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            pendingAction?.invoke()
        } else {
            setStatus("Без разрешения на Bluetooth приложение работать не может")
        }
        pendingAction = null
    }

    // ------------------------------------------------------------ поиск устройств

    /**
     * Сначала ищем по UUID сервиса. Если пусто — повторяем без фильтра и
     * отбираем по имени: 128-битный UUID вместе с именем не всегда влезает
     * в 31 байт рекламного пакета и может уехать в scan response, а часть
     * прошивок телефонов такие пакеты по фильтру не отдаёт.
     */
    @SuppressLint("MissingPermission")
    private fun startScan(byName: Boolean = false) {
        val mgr = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = mgr.adapter
        if (adapter == null || !adapter.isEnabled) {
            setStatus("Включите Bluetooth")
            return
        }
        val scanner = adapter.bluetoothLeScanner ?: return
        if (scanning) return

        if (!byName) {
            seen.clear()
            deviceList.removeAllViews()
        }
        matchByName = byName
        setStatus(if (byName) "Ищу по имени…" else "Ищу устройство…")
        scanning = true

        val filters = if (byName) emptyList()
                      else listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build())
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(filters, settings, scanCallback)

        ui.postDelayed({
            if (!scanning) return@postDelayed
            scanning = false
            scanner.stopScan(scanCallback)
            when {
                seen.isNotEmpty() -> setStatus("Найдено: ${seen.size}. Выберите из списка.")
                !byName -> startScan(byName = true)
                else -> setStatus("Устройство не найдено. Оно рядом и включено?")
            }
        }, 8000)
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val dev = result.device ?: return
            if (seen.containsKey(dev.address)) return

            val name = try { dev.name } catch (e: SecurityException) { null }
                ?: result.scanRecord?.deviceName
            if (matchByName && name?.startsWith("Solar-") != true) return
            seen[dev.address] = dev
            val b = Button(this@MainActivity)
            b.text = "${name ?: "устройство"}  (${dev.address}, ${result.rssi} дБм)"
            b.setOnClickListener { connect(dev) }
            deviceList.addView(b)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            setStatus("Поиск не удался, код $errorCode")
        }
    }

    // ------------------------------------------------------------ соединение

    @SuppressLint("MissingPermission")
    private fun connect(dev: BluetoothDevice) {
        closeGatt()
        setStatus("Подключаюсь к ${dev.address}…")
        gatt = dev.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        gatt?.let { try { it.disconnect(); it.close() } catch (_: SecurityException) {} }
        gatt = null
        queue.clear()
        busy = false
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, statusCode: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                setStatus("Подключено, читаю сервисы…")
                g.requestMtu(185)
            } else {
                setStatus("Соединение закрыто")
                ui.post { wifiBlock.visibility = View.GONE }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, statusCode: Int) {
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, statusCode: Int) {
            val svc = g.getService(SVC)
            if (svc == null) {
                setStatus("Это не наш модуль: сервиса нет")
                return
            }
            setStatus("Готово к настройке")
            ui.post { wifiBlock.visibility = View.VISIBLE }
            enqueue { subscribeStatus(g, svc) }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, s: Int) = next()
        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, s: Int) = next()

        // Android 13 и новее
        override fun onCharacteristicChanged(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray
        ) = handleLine(String(value))

        // Android 12 и старше
        @Deprecated("оставлено ради совместимости со старыми версиями Android")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            handleLine(String(c.value ?: return))
        }
    }

    @SuppressLint("MissingPermission")
    private fun subscribeStatus(g: BluetoothGatt, svc: BluetoothGattService) {
        val ch = svc.getCharacteristic(STAT) ?: return next()
        g.setCharacteristicNotification(ch, true)
        val cccd = ch.getDescriptor(CCCD) ?: return next()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
        }
    }

    // ------------------------------------------------------------ обмен

    private fun enqueue(op: () -> Unit) {
        queue.add(op)
        if (!busy) next()
    }

    private fun next() {
        val op = queue.poll()
        if (op == null) { busy = false; return }
        busy = true
        ui.post(op)
    }

    @SuppressLint("MissingPermission")
    private fun write(uuid: UUID, data: ByteArray) {
        val g = gatt ?: return next()
        val ch = g.getService(SVC)?.getCharacteristic(uuid) ?: return next()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION")
            run {
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ch.value = data
                g.writeCharacteristic(ch)
            }
        }
    }

    private fun writeCtrl(cmd: Byte) = write(CTRL, byteArrayOf(cmd))

    private fun saveCredentials() {
        val ssid = ssidEdit.text.toString()
        if (ssid.isEmpty()) { setStatus("Введите имя сети"); return }
        setStatus("Отправляю настройки…")
        enqueue { write(SSID, ssid.toByteArray()) }
        enqueue { write(PASS, passEdit.text.toString().toByteArray()) }
        enqueue { writeCtrl(CMD_APPLY) }
    }

    /** Разбор строк статуса: STATE|…|ip и NET|rssi|открытая|имя */
    private fun handleLine(line: String) {
        val parts = line.split("|")
        when (parts.getOrNull(0)) {
            "STATE" -> {
                val text = parts.getOrNull(1) ?: ""
                val ip = parts.getOrNull(2) ?: ""
                setStatus(if (ip.isNotEmpty() && ip != "0.0.0.0") "$text — http://$ip" else text)
            }
            "NET" -> {
                if (parts.getOrNull(1) == "END") return
                val rssi = parts.getOrNull(1) ?: ""
                val open = parts.getOrNull(2) == "1"
                val name = parts.drop(3).joinToString("|")   // в имени может быть «|»
                if (name.isEmpty()) return
                ui.post {
                    val b = Button(this)
                    b.text = "$name   $rssi дБм" + if (open) "   (открытая)" else ""
                    b.setOnClickListener { ssidEdit.setText(name) }
                    netList.addView(b)
                }
            }
        }
        ui.post {
            log.text = (line + "\n" + log.text).take(1200)
        }
    }

    private fun setStatus(text: String) = ui.post { status.text = text }
}
