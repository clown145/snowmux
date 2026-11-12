#if FREEINK_DEVICE_XUESU_300

#include <BleKeyboardHost.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <PersistableStore.h>
#include <esp_crc.h>
#include <store/config/ble_store_config.h>

#include <array>
#include <cstring>

namespace {
constexpr char kKeysPath[] = "/.crosspoint/xuesu-ble-keys.bin";
constexpr char kHidPath[] = "/.crosspoint/xuesu-ble-hid.bin";
constexpr char kKeysTemp[] = "/.crosspoint/xuesu-ble-keys.bin.tmp";
constexpr char kHidTemp[] = "/.crosspoint/xuesu-ble-hid.bin.tmp";
constexpr uint32_t kKeysMagic = 0x314B4258;  // XBK1
constexpr uint32_t kHidMagic = 0x31484258;   // XBH1
constexpr uint8_t kMaxBonds = freeink::BleKeyboardHost::kMaxBonds;
constexpr uint8_t kMaxKeys = 2 * kMaxBonds;
constexpr std::array kSecurityTypes{BLE_STORE_OBJ_TYPE_OUR_SEC, BLE_STORE_OBJ_TYPE_PEER_SEC};

struct Header {
  uint32_t magic;
  uint16_t recordSize;
  uint16_t count;
};
struct SecurityRecord {
  uint8_t type;
  ble_store_value_sec value;
};

bool readHeader(HalFile& file, Header& header, uint32_t magic, size_t recordSize, size_t maxRecords) {
  return file.read(&header, sizeof(header)) == sizeof(header) && header.magic == magic &&
         header.recordSize == recordSize && header.count <= maxRecords &&
         file.fileSize() == sizeof(header) + header.count * recordSize + sizeof(uint32_t);
}

bool validSecurityRecord(const SecurityRecord& record) {
  return (record.type == BLE_STORE_OBJ_TYPE_OUR_SEC || record.type == BLE_STORE_OBJ_TYPE_PEER_SEC) &&
         record.value.peer_addr.type <= BLE_ADDR_RANDOM_ID;
}

bool readSecurityRecord(HalFile& file, SecurityRecord& record) {
  return file.read(&record, sizeof(record)) == sizeof(record) && validSecurityRecord(record);
}

bool restoreKeys() {
  HalStorage::StorageLock lock;
  if (!PersistableStoreBase::recoverTempFile(kKeysPath)) return true;
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BLE", kKeysPath, file) ||
      !readHeader(file, header, kKeysMagic, sizeof(SecurityRecord), kMaxKeys))
    return false;

  uint32_t crc = 0;
  SecurityRecord record{};
  for (uint16_t i = 0; i < header.count; ++i) {
    if (!readSecurityRecord(file, record)) return false;
    crc = esp_crc32_le(crc, reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  }
  uint32_t storedCrc = 0;
  if (file.read(&storedCrc, sizeof(storedCrc)) != sizeof(storedCrc) || storedCrc != crc ||
      !file.seekSet(sizeof(header)))
    return false;

  for (uint16_t i = 0; i < header.count; ++i) {
    if (!readSecurityRecord(file, record)) return false;
    ble_store_value value{};
    value.sec = record.value;
    if (ble_store_config_write(record.type, &value) != 0) return false;
  }
  return true;
}

bool writeKeySnapshot() {
  HalStorage::StorageLock lock;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  PersistableStoreBase::recoverTempFile(kKeysPath);
  bool complete = [&] {
    HalFile file;
    if (!Storage.openFileForWrite("BLE", kKeysTemp, file)) return false;
    Header header{kKeysMagic, sizeof(SecurityRecord), 0};
    if (file.write(&header, sizeof(header)) != sizeof(header)) return false;

    uint32_t crc = 0;
    for (const int type : kSecurityTypes) {
      ble_store_key key{};
      key.sec.peer_addr = {};  // NimBLE's wildcard address is all zeroes.
      for (uint8_t i = 0; i < kMaxBonds; ++i) {
        key.sec.idx = i;
        ble_store_value value{};
        const int result = ble_store_config_read(type, &key, &value);
        if (result == BLE_HS_ENOENT) break;
        if (result != 0) return false;
        SecurityRecord record{};
        record.type = type;
        record.value = value.sec;
        if (file.write(&record, sizeof(record)) != sizeof(record)) return false;
        crc = esp_crc32_le(crc, reinterpret_cast<const uint8_t*>(&record), sizeof(record));
        ++header.count;
      }
    }
    if (file.write(&crc, sizeof(crc)) != sizeof(crc) || !file.seekSet(0) ||
        file.write(&header, sizeof(header)) != sizeof(header))
      return false;
    file.flush();
    return true;
  }();
  if (!complete) {
    Storage.remove(kKeysTemp);
    return false;
  }
  return PersistableStoreBase::replaceStagedFile(kKeysPath, kKeysTemp);
}

bool storesSecurityKey(const int type) {
  return type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC;
}

int storeWrite(const int type, const ble_store_value* value) {
  const int result = ble_store_config_write(type, value);
  if (result == 0 && storesSecurityKey(type) && !writeKeySnapshot()) {
    LOG_ERR("BLE", "Could not persist BLE keys to SD");
  }
  return result;
}

int storeDelete(const int type, const ble_store_key* key) {
  const int result = ble_store_config_delete(type, key);
  if (result == 0 && storesSecurityKey(type) && !writeKeySnapshot()) {
    LOG_ERR("BLE", "Could not persist removed BLE keys to SD");
  }
  return result;
}

bool validDevice(const freeink::PairedHidDevice& device) {
  if (device.addr[17] != '\0' || device.name[sizeof(device.name) - 1] != '\0' || device.addrType > BLE_ADDR_RANDOM_ID)
    return false;
  for (unsigned i = 0; i < 17; ++i) {
    const char c = device.addr[i];
    if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}
}  // namespace

extern "C" void __real_ble_store_config_init();
extern "C" void __wrap_ble_store_config_init() {
  __real_ble_store_config_init();
  if (!restoreKeys()) {
    LOG_ERR("BLE", "Invalid BLE key store; pairing may need to be repeated");
    __real_ble_store_config_init();
  }
  ble_hs_cfg.store_write_cb = storeWrite;
  ble_hs_cfg.store_delete_cb = storeDelete;
}

uint8_t loadXuesuHidBonds(freeink::PairedHidDevice* bonds) {
  HalStorage::StorageLock lock;
  if (!PersistableStoreBase::recoverTempFile(kHidPath)) return 0;
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BLE", kHidPath, file) ||
      !readHeader(file, header, kHidMagic, sizeof(*bonds), kMaxBonds))
    return 0;
  const size_t bytes = header.count * sizeof(*bonds);
  uint32_t storedCrc = 0;
  if (file.read(bonds, bytes) != static_cast<int>(bytes) ||
      file.read(&storedCrc, sizeof(storedCrc)) != sizeof(storedCrc) ||
      storedCrc != esp_crc32_le(0, reinterpret_cast<const uint8_t*>(bonds), bytes))
    return 0;
  for (uint16_t i = 0; i < header.count; ++i) {
    if (!validDevice(bonds[i])) return 0;
  }
  return header.count;
}

void saveXuesuHidBonds(const freeink::PairedHidDevice* bonds, const uint8_t count) {
  HalStorage::StorageLock lock;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return;
  PersistableStoreBase::recoverTempFile(kHidPath);
  const bool complete = [&] {
    HalFile file;
    if (!Storage.openFileForWrite("BLE", kHidTemp, file)) return false;
    const Header header{kHidMagic, sizeof(*bonds), count};
    const size_t bytes = count * sizeof(*bonds);
    const uint32_t crc = esp_crc32_le(0, reinterpret_cast<const uint8_t*>(bonds), bytes);
    if (file.write(&header, sizeof(header)) != sizeof(header) || file.write(bonds, bytes) != bytes ||
        file.write(&crc, sizeof(crc)) != sizeof(crc))
      return false;
    file.flush();
    return true;
  }();
  if (!complete) {
    Storage.remove(kHidTemp);
    LOG_ERR("BLE", "Could not write HID pairing list to SD");
    return;
  }
  if (!PersistableStoreBase::replaceStagedFile(kHidPath, kHidTemp)) {
    LOG_ERR("BLE", "Could not commit HID pairing list to SD");
  }
}

#endif
