"""Exercise failed SD commits and retries with production store methods."""

from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class SaveRollbackTest(unittest.TestCase):
    def test_failed_save_leaves_committed_state_and_can_be_retried(self):
        persist = (ROOT / 'lib/Serialization/PersistableStore.cpp').read_text(encoding='utf-8')
        stats = (ROOT / 'src/ReadingStatsStore.cpp').read_text(encoding='utf-8')
        wifi = (ROOT / 'src/WifiCredentialStore.cpp').read_text(encoding='utf-8')
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
struct HalStorage { struct StorageLock { ~StorageLock() {} }; };
constexpr char ROLLBACK_SUFFIX[] = ".rollback";
struct {
 std::map<std::string,std::string> files;
 std::string reject;
 bool exists(const char* path) const { return files.contains(path); }
 bool remove(const char* path) { return files.erase(path) > 0; }
 bool rename(const char* from,const char* to) {
  if (from==reject || !exists(from) || exists(to)) return false;
  files[to]=files.at(from);
  files.erase(from);
  return true;
 }
} Storage;
template<class T> auto makeUniqueNoThrow(size_t size) { return std::make_unique<T>(size); }
struct PersistableStoreBase {
 static bool recoverTempFile(const char*);
 static bool replaceStagedFile(const char*,const char*);
};
struct ReadingDayStats { uint32_t dayOrdinal; uint64_t readingMs; };
struct ReadingStatsStore {
 struct Book { std::vector<ReadingDayStats> readingDays; uint64_t totalReadingMs=0; };
 std::vector<Book> books{Book{}};
 bool dirty=false,ioWorks=false;
 unsigned long dirtySinceMs=0;
 uint64_t storedMs=0;
 size_t findBookIndexByPath(const std::string&) const { return 0; }
 void rebuildAggregatedReadingDays() {}
 void markDirty() { dirty=true; dirtySinceMs=200; }
 bool saveToFile() { if (ioWorks) storedMs=books[0].totalReadingMs; return ioWorks; }
 bool adjustBookReadingTime(const std::string&,uint32_t,int32_t);
};
struct WifiCredential { std::string ssid,password; };
struct JsonDocument { std::vector<WifiCredential> credentials; std::string last; };
void encodeCredentials(JsonDocument& doc,const std::vector<WifiCredential>& items,const std::string& last) {
 doc={items,last};
}
struct WifiCredentialStore {
 std::vector<WifiCredential> credentials{{"one","test"},{"two","test"}};
 std::string lastConnectedSsid="one";
 std::mutex credentialMutex,storeMutex;
 JsonDocument stored{credentials,lastConnectedSsid};
 bool ioWorks=false;
 const char* getFilePath() const { return "/wifi.json"; }
 bool writeDocToFile(const char*,const JsonDocument& doc,bool keepPrevious) {
  assert(keepPrevious);
  if (ioWorks) stored=doc;
  return ioWorks;
 }
 bool commitCredentials(std::vector<WifiCredential>&&,std::string&&);
 bool removeCredential(const std::string&);
};
''' + method(persist, 'bool PersistableStoreBase::recoverTempFile(')
            + method(persist, 'bool PersistableStoreBase::replaceStagedFile(')
            + method(stats, 'bool ReadingStatsStore::adjustBookReadingTime(')
            + method(wifi, 'bool WifiCredentialStore::commitCredentials(')
            + method(wifi, 'bool WifiCredentialStore::removeCredential(') + r'''
int main() {
 Storage.files={{"/state","old"},{"/state.tmp","new"}};
 Storage.reject="/state.tmp";
 assert(!PersistableStoreBase::replaceStagedFile("/state","/state.tmp"));
 assert(Storage.files.size()==1 && Storage.files.at("/state")=="old");
 Storage.files={{"/state.rollback","old"},{"/state.tmp","new"}};
 assert(PersistableStoreBase::recoverTempFile("/state"));
 assert(Storage.files.at("/state")=="old");
 Storage.reject.clear();
 assert(PersistableStoreBase::replaceStagedFile("/state","/state.tmp"));
 assert(Storage.files.size()==1 && Storage.files.at("/state")=="new");

 ReadingStatsStore stats;
 constexpr int amount=15*60*1000;
 assert(!stats.adjustBookReadingTime("book",1,amount));
 assert(!stats.dirty && stats.books[0].readingDays.empty() && stats.books[0].totalReadingMs==0);
 stats.ioWorks=true;
 assert(stats.adjustBookReadingTime("book",1,amount));
 assert(stats.storedMs==amount && stats.books[0].readingDays[0].readingMs==amount);
 stats.ioWorks=false;
 assert(!stats.adjustBookReadingTime("book",1,-amount));
 assert(stats.books[0].readingDays.size()==1 && stats.books[0].readingDays[0].readingMs==amount);

 WifiCredentialStore wifi;
 assert(!wifi.removeCredential("one"));
 assert(wifi.credentials.size()==2 && wifi.lastConnectedSsid=="one");
 assert(wifi.stored.credentials.size()==2 && wifi.stored.last=="one");
 wifi.ioWorks=true;
 assert(wifi.removeCredential("one"));
 assert(wifi.credentials.size()==1 && wifi.credentials.front().ssid=="two");
 assert(wifi.stored.credentials.size()==1 && wifi.lastConnectedSsid.empty() && wifi.stored.last.empty());
}
''')


if __name__ == '__main__':
    unittest.main()
