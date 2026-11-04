// Flash resident blocklist for ESP32-C3
//
// The PSRAM build keeps every blocked domain as a NUL separated string in a
// multi megabyte arena. An ESP32-C3 has no PSRAM and roughly 400KB of SRAM, so
// this implementation stores a sorted array of 64-bit FNV-1a domain hashes in
// the data partition and binary searches the file for each query. The 64-bit
// hash keeps the false positive rate near 1e-14 per query for a 100k entry
// list, well below the failure rate of a typical home link.
//
// Layout of the data partition during a rebuild:
//   blraw.bin   unsorted hashes appended while the list downloads
//   blhash.bin  sorted, de-duplicated hashes that queries search
// The previous blhash.bin is dropped once the download completes and before
// blhash.bin.tmp is written, so a rebuild needs room for two copies of the
// list, not three.

#include "appGlobals.h"

#if BL_FLASH_INDEX

static File blFile;               // sorted hash file, searched by the DNS task
static File blRaw;                // raw hash file, written while downloading
static SemaphoreHandle_t blMutex; // guards blFile between the DNS and web tasks
static uint32_t blItems;          // hashes in blhash.bin
static uint32_t blCap;            // max hashes the data partition can hold
static uint32_t blLimit;          // maxDomains setting, 0 means no extra limit
static uint32_t blRawCount;       // hashes appended to blraw.bin
static uint32_t blGen;            // bumped whenever the rules change

static uint64_t customAdd[BL_CUSTOM_MAX];
static uint64_t customDel[BL_CUSTOM_MAX];
static uint16_t customAddCnt, customDelCnt;

static bool customHas(const uint64_t* set, uint16_t cnt, uint64_t h) {
  for (uint16_t i = 0; i < cnt; i++) if (set[i] == h) return true;
  return false;
}

static void normaliseName(char* name) {
  // match the loader: lower case and no leading www.
  for (char* p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
  if (!strncmp(name, "www.", 4)) memmove(name, name + 4, strlen(name + 4) + 1);
}

uint64_t blHash(const char* name) {
  // FNV-1a, 64 bit. Input must already be normalised (lower case, no www.)
  uint64_t h = 14695981039346656037ULL;
  for (const char* p = name; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 1099511628211ULL;
  }
  return h;
}

uint32_t blCount() { return blItems; }

uint32_t blGeneration() { return blGen; }

size_t blStoreBytes() { return (size_t)blItems * sizeof(uint64_t); }

uint32_t blCapacity() {
  if (!blCap) {
    size_t total = STORAGE.totalBytes();
    size_t reserve = BL_FS_RESERVE;
    if (total < reserve * 2) reserve = total / 4; // small / custom partitions
    // raw and sorted copies coexist, so only half the free space is usable
    blCap = (uint32_t)(((total - reserve) / 2) / sizeof(uint64_t));
  }
  if (blLimit && blCap > blLimit) return blLimit;
  return blCap;
}

void blSetLimit(uint32_t limit) { blLimit = limit; }

bool blInit() {
  if (!blMutex) blMutex = xSemaphoreCreateMutex();
  if (!blMutex) {
    LOG_ERR("Blocklist: mutex not created");
    return false;
  }
  // drop leftovers from a build that was interrupted by a reset
  char tmpPath[FILE_NAME_LEN];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", BL_HASH_FILE);
  if (STORAGE.exists(tmpPath)) STORAGE.remove(tmpPath);
  if (STORAGE.exists(BL_RAW_FILE)) STORAGE.remove(BL_RAW_FILE);
  if (!STORAGE.exists(BL_HASH_FILE)) {
    File f = STORAGE.open(BL_HASH_FILE, FILE_WRITE);
    if (!f) {
      LOG_ERR("Blocklist: cannot create %s", BL_HASH_FILE);
      return false;
    }
    f.close();
  }
  blFile = STORAGE.open(BL_HASH_FILE, FILE_READ);
  if (!blFile) {
    LOG_ERR("Blocklist: cannot open %s", BL_HASH_FILE);
    return false;
  }
  blItems = blFile.size() / sizeof(uint64_t);
  return true;
}

bool blBeginBuild() {
  blDiscardBuild();
  blRaw = STORAGE.open(BL_RAW_FILE, FILE_WRITE);
  if (!blRaw) {
    LOG_ERR("Blocklist: cannot create %s", BL_RAW_FILE);
    return false;
  }
  blRawCount = 0;
  return true;
}

bool blAddHash(uint64_t h) {
  if (blRawCount >= blCapacity()) return false;
  if (!blRaw) return false;
  if (blRaw.write((const uint8_t*)&h, sizeof(h)) != sizeof(h)) return false;
  blRawCount++;
  return true;
}

void blDiscardBuild() {
  if (blRaw) blRaw.close();
  blRawCount = 0;
  if (STORAGE.exists(BL_RAW_FILE)) STORAGE.remove(BL_RAW_FILE);
}

static int cmpHash(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
  return (x > y) - (x < y);
}

bool blCommitBuild(uint32_t& rawCount) {
  rawCount = 0;
  if (blRaw) blRaw.close();
  File raw = STORAGE.open(BL_RAW_FILE, FILE_READ);
  if (!raw) {
    LOG_ERR("Blocklist: %s missing after download", BL_RAW_FILE);
    return false;
  }

  uint32_t n = raw.size() / sizeof(uint64_t);
  if (n == 0) {
    raw.close();
    STORAGE.remove(BL_RAW_FILE);
    LOG_WRN("Blocklist: download contained no domains");
    return false;
  }
  rawCount = n;

  // Pick a bucket count that keeps one bucket buffer inside a third of the
  // free heap. Fewer buckets means fewer passes over the raw file, so start
  // small and only split further when the buffer would not fit.
  uint32_t buckets = 8;
  while (buckets < 256 &&
         (n / buckets + 64) * sizeof(uint64_t) > ESP.getFreeHeap() / 3)
    buckets <<= 1;
  uint8_t bits = 0;
  for (uint32_t b = buckets; b > 1; b >>= 1) bits++;
  uint8_t shift = 64 - bits;

  uint32_t counts[256];
  memset(counts, 0, sizeof(counts));
  uint64_t h;
  uint32_t total = 0;
  raw.seek(0);
  while (raw.read((uint8_t*)&h, sizeof(h)) == sizeof(h)) {
    counts[h >> shift]++;
    if ((++total & 0x7FFF) == 0) vTaskDelay(1);
  }

  uint32_t maxCount = 0;
  for (uint32_t b = 0; b < buckets; b++)
    if (counts[b] > maxCount) maxCount = counts[b];

  uint64_t* buf = (uint64_t*)malloc((maxCount + 1) * sizeof(uint64_t));
  if (!buf) {
    raw.close();
    LOG_ERR("Blocklist: no heap for a %lu entry sort bucket",
            (unsigned long)maxCount);
    return false;
  }

  // The previous generation is dropped before the new file is written: both
  // copies cannot coexist inside a 4MB flash partition.
  if (xSemaphoreTake(blMutex, portMAX_DELAY) == pdTRUE) {
    if (blFile) blFile.close();
    if (STORAGE.exists(BL_HASH_FILE)) STORAGE.remove(BL_HASH_FILE);
    blItems = 0;
    xSemaphoreGive(blMutex);
  }

  char tmpPath[FILE_NAME_LEN];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", BL_HASH_FILE);
  STORAGE.remove(tmpPath);
  File out = STORAGE.open(tmpPath, FILE_WRITE);
  if (!out) {
    free(buf);
    raw.close();
    LOG_ERR("Blocklist: cannot create %s", tmpPath);
    return false;
  }

  uint32_t unique = 0;
  uint32_t t0 = millis();
  bool ok = true;
  for (uint32_t b = 0; b < buckets && ok; b++) {
    raw.seek(0);
    size_t cnt = 0;
    uint32_t seen = 0;
    while (raw.read((uint8_t*)&h, sizeof(h)) == sizeof(h)) {
      if ((h >> shift) == b && cnt <= maxCount) buf[cnt++] = h;
      if ((++seen & 0x7FFF) == 0) vTaskDelay(1); // feed the task watchdog
    }
    qsort(buf, cnt, sizeof(uint64_t), cmpHash);
    for (size_t i = 0; i < cnt; i++) {
      if (i && buf[i] == buf[i - 1]) continue; // duplicate in this bucket
      if (out.write((const uint8_t*)&buf[i], sizeof(uint64_t)) != sizeof(uint64_t)) {
        ok = false;
        break;
      }
      unique++;
    }
    char prog[16];
    snprintf(prog, sizeof(prog), "Sort %u%%", (unsigned)((b + 1) * 100 / buckets));
    updateConfigVect("loadProg", prog);
    vTaskDelay(1);
  }
  free(buf);
  out.close();
  raw.close();

  if (!ok) {
    STORAGE.remove(tmpPath);
    LOG_ERR("Blocklist: write failed, previous list dropped");
    return false;
  }

  bool renamed = false;
  if (xSemaphoreTake(blMutex, portMAX_DELAY) == pdTRUE) {
    renamed = STORAGE.rename(tmpPath, BL_HASH_FILE);
    if (renamed) {
      blFile = STORAGE.open(BL_HASH_FILE, FILE_READ);
      if (blFile) {
        blItems = unique;
        blGen++;
      }
      else renamed = false;
    }
    xSemaphoreGive(blMutex);
  }
  STORAGE.remove(BL_RAW_FILE);
  if (!renamed) {
    LOG_ERR("Blocklist: could not activate %s", BL_HASH_FILE);
    return false;
  }
  LOG_INF("Blocklist: %lu domains, %lu duplicates, %s sorted in %lus",
          (unsigned long)unique, (unsigned long)(n - unique),
          fmtSize(blStoreBytes()), (unsigned long)((millis() - t0) / 1000));
  return true;
}

bool blIsBlocked(uint64_t h) {
  // custom deletes win over custom adds, which win over the downloaded list
  if (customHas(customDel, customDelCnt, h)) return false;
  if (customHas(customAdd, customAddCnt, h)) return true;
  if (!blMutex) return false;
  bool found = false;
  if (xSemaphoreTake(blMutex, portMAX_DELAY) == pdTRUE) {
    if (blFile && blItems) {
      uint32_t lo = 0, hi = blItems - 1;
      while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t v = 0;
        if (!blFile.seek(mid * sizeof(uint64_t)) ||
            blFile.read((uint8_t*)&v, sizeof(v)) != sizeof(v))
          break;
        if (v == h) {
          found = true;
          break;
        }
        if (v < h) lo = mid + 1;
        else if (mid) hi = mid - 1;
        else break;
      }
    }
    xSemaphoreGive(blMutex);
  }
  return found;
}

void blShow(uint32_t maxItems) {
  // domain names are not stored on C3, only their hashes
  if (!maxItems) maxItems = blItems;
  if (maxItems > blItems) maxItems = blItems;
  LOG_SEND("Blocklist: %lu hashes (%s), showing %lu\n", (unsigned long)blItems,
           fmtSize(blStoreBytes()), (unsigned long)maxItems);
  if (!blMutex || xSemaphoreTake(blMutex, portMAX_DELAY) != pdTRUE) return;
  for (uint32_t i = 0; i < maxItems && blFile; i++) {
    uint64_t v = 0;
    if (!blFile.seek(i * sizeof(uint64_t))) break;
    if (blFile.read((uint8_t*)&v, sizeof(v)) != sizeof(v)) break;
    LOG_SEND("%lu: %016llx\n", (unsigned long)i, (unsigned long long)v);
  }
  xSemaphoreGive(blMutex);
}

bool blCustomLoad() {
  customAddCnt = customDelCnt = 0;
  blGen++;
  if (!STORAGE.exists(CUSTOM_FILE_PATH)) {
    File f = STORAGE.open(CUSTOM_FILE_PATH, FILE_WRITE);
    if (f) f.close();
    else LOG_WRN("Failed to create file %s", CUSTOM_FILE_PATH);
    return true;
  }
  File file = STORAGE.open(CUSTOM_FILE_PATH, FILE_READ);
  if (!file) {
    LOG_ERR("Failed to open %s", CUSTOM_FILE_PATH);
    return false;
  }
  char domName[IN_FILE_NAME_LEN];
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    bool add = true;
    if (line.charAt(0) == '#') { // deletion
      add = false;
      line.remove(0, 1);
    }
    line.toCharArray(domName, sizeof(domName));
    normaliseName(domName);
    uint64_t h = blHash(domName);
    if (add) {
      if (customAddCnt < BL_CUSTOM_MAX) customAdd[customAddCnt++] = h;
      else LOG_WRN("Custom blocklist full, %s ignored", domName);
    } else {
      for (uint16_t i = 0; i < customAddCnt; i++)
        if (customAdd[i] == h) {
          customAdd[i] = customAdd[--customAddCnt];
          break;
        }
      if (customDelCnt < BL_CUSTOM_MAX) customDel[customDelCnt++] = h;
      else LOG_WRN("Custom blocklist full, %s ignored", domName);
    }
  }
  file.close();
  return true;
}

bool blCustomAdd(uint64_t h) {
  for (uint16_t i = 0; i < customDelCnt; i++)
    if (customDel[i] == h) {
      customDel[i] = customDel[--customDelCnt];
      break;
    }
  if (customHas(customAdd, customAddCnt, h)) return true;
  if (customAddCnt >= BL_CUSTOM_MAX) {
    LOG_WRN("Custom blocklist full (%d entries)", BL_CUSTOM_MAX);
    return false;
  }
  customAdd[customAddCnt++] = h;
  blGen++;
  return true;
}

bool blCustomDel(uint64_t h) {
  for (uint16_t i = 0; i < customAddCnt; i++)
    if (customAdd[i] == h) {
      customAdd[i] = customAdd[--customAddCnt];
      break;
    }
  if (customHas(customDel, customDelCnt, h)) return true;
  if (customDelCnt >= BL_CUSTOM_MAX) {
    LOG_WRN("Custom blocklist full (%d entries)", BL_CUSTOM_MAX);
    return false;
  }
  customDel[customDelCnt++] = h;
  blGen++;
  return true;
}

void blCustomClear() {
  customAddCnt = customDelCnt = 0;
  blGen++;
}

uint32_t blCustomAddedCount() { return customAddCnt; }

uint32_t blCustomDeletedCount() { return customDelCnt; }

#endif // BL_FLASH_INDEX
