# Design Document: Embedded LSM-Tree Key-Value Storage Engine

## Метадані проєкту

- **Проєкт**: lsm-hdv (робоча назва)
- **Тип документа**: Архітектурний дизайн-документ і стратегія комплексного тестування ПЗ
- **Статус**: На рев'ю
- **Мова і стандарт**: C++20 (без зовнішніх бібліотек часу виконання чи СУБД)
- **Специфікація / Оракули**: Алгоритмічна модель LSM-Tree (Patrick O'Neil et al., 1996), бінарний блочний формат LevelDB/RocksDB; оракули: RocksDB C++ API та in-memory модель на базі `std::map`.
- **Цільові метрики**: Алгоритмічне ядро ≥ 3 500 рядків коду (без урахування тестів і бенчмарків); Branch Coverage ядра ≥ 90%; Mutation Score ≥ 80% (для підсистем SkipList, SSTable, WAL, Compaction ≥ 85%).

---

## 1. Мета та опис проєкту

Проєкт є розробкою автономної вбудованої бібліотеки (Embedded Key-Value Storage Engine) мовою C++20, що реалізує парадигму Log-Structured Merge-tree (LSM).

Основна інженерна мета — забезпечення високої пропускної спроможності послідовного запису (high write-throughput) шляхом трансформації випадкових дискових операцій у послідовний асинхронний I/O потік (Append-Only WAL та незмінні SSTable файли).

Бібліотека компілюється як статична/динамічна бібліотека без сторонніх рантайм-залежностей і надає C++ API із семантикою get/put/delete/scan та гарантіями стійкості (Durability).

Проєкт проєктується з акцентом на комплексну верифікацію коректності: математичні інваріанти, Property-based тестування, фазинг бінарних парсерів та LLVM-мутаційне тестування граничних умов.

---

## 2. Межі проєкту (Scope & MVP)

### 2.1. Входить до MVP (In Scope)

1. **Write-Ahead Log (WAL)**:
   - Бінарне append-only логування операцій до їх фіксації в оперативній пам'яті.
   - Формат запису: CRC32 checksum, довжина ключа (`uint32`), довжина значення (`uint32`), тип операції (Put/Delete), корисне навантаження.
   - Механізм Crash Recovery Replay: послідовне відновлення стану активної MemTable після апаратного перезапуску або збою процесу із зупинкою на першому пошкодженому записі.
2. **MemTable (Concurrent SkipList)**:
   - Власна реалізація SkipList із підтримкою ймовірнісного розподілу висоти вузлів (параметр *p* = 0.25, MaxLevel = 16).
   - Підтримка операцій `Put`, `Get`, `Delete` (маркери Tombstone) та двонаправлених ітераторів.
   - Політика заповнення (поріг пам'яті 4 МБ): переведення активної MemTable в Immutable MemTable з ініціалізацією фонового скидання на диск (Flush).
3. **SSTable Storage & Block Encoding (Sorted String Table)**:
   - Незмінні (Immutable) файли на диску з фіксованими Data Blocks (по 4 КБ) та Index Block.
   - Префіксне стиснення / Delta Encoding для ключів всередині блоків даних для мінімізації розміру.
   - **Bloom Filter**: власний бітовий фільтр для кожного SSTable файлу з *k* хеш-функціями для відсікання зайвих звернень до диска з ймовірністю хибнопозитивного спрацьовування < 1%.
   - **Block Cache**: LRU-кеш нестиснених блоків у пам'яті для оптимізації частих читань.
4. **Compaction Engine (Дворівневе злиття)**:
   - Механізм фонового Size-Tiered / Leveled Compaction на базі алгоритму K-way Merge (пріоритетна черга з мінімальним елементом `std::priority_queue`).
   - Фізичне видалення застарілих версій ключів та записів, помічених маркерами Tombstone.
5. **Public Engine API**:
   - Методи: `Open(Options)`, `Close()`, `Put(Slice, Slice)`, `Get(Slice) -> std::optional<std::string>`, `Delete(Slice)`, `NewIterator()` / `Scan(start_key, end_key)`.

### 2.2. Поза межами проєкту (Out of Scope)

- Мережеві протоколи, RPC, клієнт-серверна архітектура або сокети.
- Мова запитів SQL, вторинні індекси, схеми таблиць та реляційні зв'язки.
- Розподілений консенсус (Raft / Paxos) та шардування між вузлами.
- Багатоверсійне управління транзакціями (MVCC) зі знімками (Snapshot Isolation) поза межами читання актуальної версії.

### 2.3. Модель конкурентності та безпека потоків (Thread Safety Contract)

- **Багатопотоковий доступ до StorageEngine (Single-Writer / Multiple-Readers або Thread-Safe Facade):**
  - `Put`, `Delete`: потокобезпечні (Thread-safe). Захищаються внутрішнім м'ютексом на рівні запису в WAL та заміни активної MemTable. Завдяки Concurrent SkipList операції запису в пам'ять можуть бути lock-free або fine-grained locked, проте послідовність у WAL синхронізується для забезпечення лінеаризованого порядку транзакцій.
  - `Get`: повністю lock-free / non-blocking відносно читачів. Читання з MemTable та SSTables є потокобезпечним і підтримує необмежену кількість паралельних потоків без взаємного блокування.
- **Фонові потоки рушія:**
  - Один фоновий потік Flush (скидання заповненої Immutable MemTable на диск у L0 SSTable).
  - Один фоновий потік Compaction (злиття SSTables між рівнями за алгоритмом K-way Merge). Взаємодія між фоновими потоками та читачами/письменниками координується через `std::shared_mutex` або версіонування списку активних файлів (File Manifest).

---

## 3. Архітектура та життєвий цикл даних

### 3.1. Конвеєр запису (Write Pipeline)

1. Користувач викликає `db.Put(key, value)` або `db.Delete(key)`.
2. Запис серіалізується, розраховується контрольна сума CRC32, і він додається в кінець відкритого дескриптора файлу `wal.log` з опційним викликом `fsync`.
3. Запис додається до in-memory структури `MemTable` (SkipList).
4. Якщо розмір `MemTable` перевищує `options.write_buffer_size`:
   - Поточна `MemTable` позначається як `Immutable MemTable`.
   - Створюється нова активна `MemTable` та новий сегмент WAL.
   - Фоновий воркер (`std::jthread`) скидає `Immutable MemTable` у новий файл `.sst` нульового рівня (Level 0), будуючи Index Block та Bloom Filter.

### 3.2. Конвеєр читання (Read Pipeline)

1. Пошук ключа в активній `MemTable`. Якщо знайдено — повернення значення (або `NotFound`, якщо Tombstone).
2. Пошук в `Immutable MemTable` (якщо вона очікує на скидання).
3. Пошук у SSTable файлах від найновішого до найстарішого:
   - Перевірка входження ключа у `BloomFilter` SSTable-файлу (якщо 0 — файл пропускається).
   - Бінарний пошук у завантаженому `IndexBlock` для знаходження зміщення (offset) потрібного блоку даних.
   - Звернення до `LRU Block Cache`: якщо блоку немає — дискове зчитування та збереження в кеш.
   - Бінарний/лінійний пошук всередині 4 КБ блоку даних.
4. Повернення знайденого значення або помилки `Status::NotFound`.

### 3.3. Модулі ядра та оцінка обсягу коду

| Модуль | Відповідальність та алгоритмічний зміст | Оцінка (рядків коду) |
|---|---|---|
| `memtable/` | Багатозв'язний SkipList, вузли з різними рівнями покажчиків, генерація висоти, безпечні ітератори. | 650 |
| `wal/` | Append-only бінарний запис, обчислення CRC32, розбір сегментів, Crash Replay парсер. | 450 |
| `sstable/` | Генерація незмінних файлів, Delta/Varint енкодинг, бінарний Index Block, парсер SST. | 850 |
| `filter/` | Bloom Filter: подвійне хешування (Murmur3), бітова матриця, конструювання та валідація. | 300 |
| `cache/` | LRU Block Cache: комбінація хеш-таблиці та двобічно зв'язного списку з eviction політикою. | 350 |
| `compaction/` | K-way Merge ітераторів, дворівнева компакція, garbage collection застарілих версій і tombstones. | 700 |
| `db/` | Оркестрація життєвого циклу DBImpl, координування фонових потоків `std::jthread`, публічний API. | 450 |
| **Разом ядро** | **Суто алгоритмічний код (без урахування тестів і бенчмарків)** | **~3 750** |

---

## 4. Специфікація бінарного формату зберігання

### 4.1. Формат запису WAL (Write-Ahead Log Record)

Кожен запис у WAL-файлі має наступну байтову структуру:

- **CRC32** (4 байти): IEEE 802.3 контрольна сума решти полів запису.
- **Record Type** (1 байт): `0x01` для PUT, `0x02` для DELETE (Tombstone).
- **Key Length** (4 байти, Little-Endian).
- **Value Length** (4 байти, Little-Endian, для DELETE дорівнює 0).
- **Key Data** (Key Length байтів).
- **Value Data** (Value Length байтів).

### 4.2. Математичні формули підсистем

#### 4.2.1. Масштабування Bloom Filter

Оптимальна кількість хеш-функцій *k* та необхідна кількість бітів на ключ *m/n* для досягнення цільової ймовірності хибнопозитивного спрацьовування *p* ≤ 0.01 (1%):

```
m/n = -ln(p) / (ln(2))² ≈ 9.59 ⇒ 10 bits/key
k   = (m/n) * ln(2) = 10 * 0.69315 ≈ 7 hash functions
```

Для уникнення додаткових обчислень використовується подвійне хешування за формулою Кірша-Мітценмахера:

```
g_i(x) = h_1(x) + i * h_2(x)  (mod m),  i ∈ [0, k-1]
```

де *h₁(x)* та *h₂(x)* отримуються з 64-бітного значення MurmurHash3.

#### 4.2.2. Оцінка висоти SkipList

Імовірність досягнення висоти *h* для вузла при ймовірнісному параметрі *p* = 0.25:

```
P(Height = h) = (1 - p) * p^(h-1)
```

Математичне сподівання кількості порівнянь при пошуку в SkipList з *N* елементами:

```
E[Search Steps] = O(log_(1/p)(N) / p) = O(log N)
```

### 4.3. Формат файлу SSTable (.sst)

Файл складається з послідовності блоків:

1. **Data Block 0 ... N**: Масив відсортованих пар ключ-значення фіксованого розміру (за замовчуванням 4096 байтів). Включає масив зміщень (restarts) для швидкого бінарного пошуку з Delta Encoding.
2. **Filter Block**: Серіалізований Bloom Filter для всіх ключів файлу.
3. **Index Block**: Зберігає для кожного Data Block його граничний ключ (найбільший ключ у блоці) та BlockHandle (offset + size).
4. **Footer** (фіксовані 48 байтів у самому кінці файлу):
   - Filter Block Handle (offset: `uint64`, size: `uint64`).
   - Index Block Handle (offset: `uint64`, size: `uint64`).
   - Magic Number (8 байтів): константа `0xDB47534D5F4B5631` (`"LSM_KV1"`).

---

## 5. C++ API та інтерфейси

```cpp
namespace lsm {

enum class StatusCode {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}
    Status(StatusCode code, std::string_view msg) : code_(code), msg_(msg) {}

    bool ok() const { return code_ == StatusCode::kOk; }
    bool is_not_found() const { return code_ == StatusCode::kNotFound; }
    bool is_corruption() const { return code_ == StatusCode::kCorruption; }

    std::string to_string() const;

private:
    StatusCode code_;
    std::string msg_;
};

struct Options {
    size_t write_buffer_size = 4 * 1024 * 1024;     // 4 MB MemTable
    size_t block_size = 4 * 1024;                   // 4 KB Data Block
    size_t block_cache_capacity = 32 * 1024 * 1024; // 32 MB LRU Cache
    int bloom_bits_per_key = 10;                    // ~1% false positive
    bool sync_wal = false;                          // Synchronous fsync on Put
};

class Iterator {
public:
    virtual ~Iterator() = default;
    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Seek(std::string_view target) = 0;
    virtual void Next() = 0;
    virtual std::string_view key() const = 0;
    virtual std::string_view value() const = 0;
    virtual Status status() const = 0;
};

class StorageEngine {
public:
    static Status Open(const std::string& db_path, const Options& options,
                       std::unique_ptr<StorageEngine>& db);
    virtual ~StorageEngine() = default;

    virtual Status Put(std::string_view key, std::string_view value) = 0;
    virtual Status Get(std::string_view key, std::string* value) = 0;
    virtual Status Delete(std::string_view key) = 0;

    virtual std::unique_ptr<Iterator> Scan(std::string_view start_key,
                                           std::string_view end_key) = 0;
    virtual Status Sync() = 0;
};

} // namespace lsm
```

### 5.2. Поведінка Iterator при паралельних або послідовних модифікаціях (Put/Delete)

- **Семантика знімка (Snapshot-like View):**
  - При виклику `db.Scan(start_key, end_key)` або `db.NewIterator()` створюється ітератор, прив'язаний до фіксованої послідовності версій на момент відкриття.
  - Якщо інші потоки (або той самий) роблять `Put`/`Delete` після створення ітератора: ітератор не гарантує відображення нових змін, зроблених після його створення, але гарантує валідність пам'яті (Memory Safety) — він не падає в UB/Segfault і повертає консистентний зріз даних.
    *(оскільки SSTable файли на диску є Immutable, а вузли у SkipList керуються через atomic-покажчики/shared_ptr або tombstone-маркери, активний ітератор захищений від руйнування внутрішнього стану).*
- **Інвалідація під час Compaction:**
  - Файли SSTable, які читає відкритий ітератор, утримуються через `std::shared_ptr` (або механізм Reference Counting файлових дескрипторів). Навіть якщо фоновий потік компакції злив ці файли й створив нові, старі файли не видаляються фізично з диска доти, доки ітератор не буде знищено (destructor звільняє ref-count).

---

## 6. Стратегія тестування ПЗ

### 6.1. Матриця методів та інструментів тестування

| Рівень | Метод | Інструменти | Мета та об'єкт перевірки |
|---|---|---|---|
| L1 | Unit Testing | Google Test | Ізольована перевірка окремих алгоритмів (SkipList, Bloom Filter, LRU Cache, бінарне кодування). |
| L2 | Property-Based Testing | RapidCheck | Генерація тисяч випадкових операцій та перевірка математичних інваріантів. |
| L3 | Differential Testing | RocksDB C++ / `std::map` | Паралельний прогін довільних послідовностей на нашому рушії та еталонних оракулах. |
| L4 | Fuzzing | LLVM libFuzzer | Подача деформованих/битих бінарних блоків SSTable та WAL для перевірки надійності обробки помилок та відсутності UB/Segfault. |
| L5 | Fault Injection | Mock VFS / Linux syscall interception | Симуляція раптових падінь живлення (Crash Recovery) та відмов диска (ENOSPC, EIO). |
| L6 | Mutation Testing | Mull (LLVM IR) | Вимірювання живучості мутантів (інжекція помилок у бінарний пошук, обчислення CRC, compaction). |
| L7 | Dynamic Analysis & Sanitizers | Valgrind, ASan/UBSan/TSan | Перевірка відсутності витоків пам'яті, UB, гонитв даних (data races) та невалідних доступів до пам'яті під навантаженням. |
| L8 | Benchmarking & Performance | Google Benchmark, perf | Вимірювання Throughput (ops/sec) для читання/запису, латентності операцій (p50, p99) та оцінка деградації при компакції. |

### 6.2. Каталог перевірок та інваріантів (L1–L5)

#### L1: Модульні тести (Google Test)

- **U-01**: SkipList — коректність вставки, пошуку та видалення на випадкових 100 000 ключах; перевірка коректності порядку ітератора.
- **U-02**: Bloom Filter — перевірка 0% False Negatives (якщо ключ додано, фільтр ніколи не повертає false) та емпіричний вимір False Positive Rate (≤ 1.2% при 10 бітах/ключ).
- **U-03**: LRU Block Cache — витіснення найменш використовуваних сторінок при перевищенні ліміту пам'яті; повторний доступ оновлює позицію.
- **U-04**: SSTable Builder & Reader — валідація формату, читання магічного числа, правильність розпакування Delta-кодованих блоків.
- **U-05**: WAL Logger — перевірка коректності обчислення CRC32 та виявлення зміненого байтика у тілі запису.

#### L2: Property-Based тестування (RapidCheck)

- **P-01** (Round-trip інваріант блоку): `decodeBlock(encodeBlock(entries)) == entries` для будь-яких валідних відсортованих наборів ключів і значень.
- **P-02** (Інваріант читання після видалення): Для будь-якої послідовності операцій над ключем *k*: якщо останньою операцією був `Delete(k)`, то `Get(k)` гарантовано повертає `Status::NotFound`.
- **P-03** (Монотонність ітератора): Для довільних збережених даних результат виклику `Scan(start, end)` повертає послідовність *k₁ < k₂ < ... < kₙ*, де кожен ключ строго більший за попередній згідно з лексикографічним порядком.
- **P-04** (Інваріант Compaction): Множина пар ключ-значення, видима через `Scan()` до запуску процедури компакції, строго еквівалентна множині після її завершення.

#### L3: Диференційне тестування

- **D-01**: Створюється тестовий harness, який генерує випадкові ланцюжки з *N* = 50 000 операцій (Put, Get, Delete, Scan). Кожна операція паралельно надсилається до нашого LSM-рушія та до `std::map<std::string, std::string>`. Після кожної операції стан результатів та повернених значень звіряється на 100% відповідність.
- **D-02**: Звірка поведінки range scan з RocksDB при граничних умовах (пустий діапазон, діапазон за межами наявних ключів, ключі однакового префіксу).

#### L4: Фазинг (LLVM libFuzzer)

- **F-01** (SSTable Parser Fuzzing): Генерація спотворених байтових послідовностей у Data Blocks, Index Blocks та Footer. Рушій зобов'язаний повертати `Status::Corruption` і в жодному разі не допускати Heap-buffer-overflow, Segmentation fault або Undefined Behavior.
- **F-02** (WAL Replay Fuzzing): Подача обрізаних та пошкоджених файлів журналів із рандомними бітовими зсувами. Replay повинен відновлювати записи до першого битого сегмента з помилкою CRC і коректно зупинятися без падіння процесу.

#### L5: Fault Injection та Crash Consistency

- **FI-01** (Torn Write Replay): Симуляція аварійного завершення процесу посеред запису блоку WAL (запис половини байтів). Перевірка, що після виклику `Open()` відновлюються всі транзакції до моменту обриву.
- **FI-02** (Disk Full Handling): Симуляція повернення системою помилки `ENOSPC` під час скидання Immutable MemTable на диск. Рушій не повинен втрачати дані з пам'яті й зобов'язаний повернути зрозумілий статус помилки користувачу.

#### L6: Мутаційне тестування (Mull)

- **M-01** (SkipList Boundaries): Інжекція мутацій в оператори порівняння (`<` на `<=`, пропуск оновлення рівнів `forward[i]`). Тести U-01 та P-03 зобов'язані впасти.
- **M-02** (Bloom Filter Logic): Зміна коефіцієнтів у формулі подвійного хешування Кірша-Мітценмахера або масок бітового масиву. Тест U-02 зобов'язаний зафіксувати сплеск False Negatives.
- **M-03** (Binary Search in SST): Мутація умов виходу (`low <= high`) та розрахунку середини блоку `mid`. Тести P-01 та U-04 зобов'язані провалитися.
- **M-04** (Tombstone Pruning): Видалення умови перевірки маркера видалення під час K-way merge. Тести P-02 та P-04 зобов'язані зафіксувати повернення видаленого ключа.

#### L7: Динамічний аналіз та Concurrency (ASan, TSan, UBSan)

- **S-01** (Memory Sanitization): Прогін усього набору тестів під AddressSanitizer та LeakSanitizer — відсутність use-after-free, heap-buffer-overflow та витоків пам'яті.
- **S-02** (Data Race Detection): Запуск 16 паралельних потоків (суміш Put, Get, Delete та активних ітераторів Scan) під ThreadSanitizer — 0 гонитв даних при доступі до MemTable та файлових дескрипторів.
- **S-03** (Undefined Behavior): Виконання операцій бінарної серіалізації чисел Little-Endian та розрахунку CRC32 під UBSan — відсутність знакових переповнень і невалідних бітових зсувів.

#### L8: Продуктивність та регресія (Google Benchmark)

- **B-01** (Sequential Write Throughput): Вимірювання пропускної спроможності запису батчами (цільовий throughput: ≥ 150 000 ops/sec).
- **B-02** (Random Read Latency): Оцінка затримок випадкового читання з холодного та гарячого кешу (вимірювання метрик p50, p99).
- **B-03** (Compaction Overhead): Оцінка впливу фонового K-way merge на деградацію затримок читання.

---

## 7. Гейти якості (Quality Gates & CI/CD)

- **G-01 (Coverage Gate)**: Branch Coverage ядра ≥ 90%, Line Coverage ≥ 92% (вимірюється через llvm-cov / lcov).
- **G-02 (Mutation Score Gate)**: Загальний показник стійкості до мутацій у Mull ≥ 80% (для компонентів `sstable/`, `memtable/`, `wal/` ≥ 85%). Кожен мутант, що вижив, повинен бути або вбитий новим тестом, або документований як еквівалентний.
- **G-03 (Sanitizers Clean)**: 100% тестів проходять перевірку Valgrind, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan) та ThreadSanitizer (TSan) без жодного попередження.
- **G-04 (Non-crashing Fuzzing)**: Щонайменше 1 година безперервного фазингу libFuzzer на корпусі пошкоджених файлів без падінь.

---

## 8. Графік реалізації (Milestones)

```
M0: Каркас проєкту & CI
 └──> M1: MemTable & WAL (RapidCheck)
       └──> M2: SSTable, Bloom Filter & LRU
             └──> M3: Compaction & API (Differential)
                   └──> M4: Fuzzing & Core Size Verification
                         └──> M5: Mutation Testing & Final Release
```

- **M0**: Каркас проєкту CMake/CTest, інтеграція Google Test, Clang Sanitizers та CI на GitHub Actions.
- **M1**: Реалізація `MemTable` (SkipList) та бінарного `WAL` з тестами інваріантів RapidCheck (P-01, P-02).
- **M2**: Реалізація `SSTable Builder`, `Block Parser`, `Bloom Filter`, `LRU Cache`.
- **M3**: Реалізація `Compaction Engine` (K-way merge), публічний API `Scan`/`Put`/`Get`/`Delete`, диференційні тести проти `std::map`, багатопотокові стрес-тести під TSan.
- **M4**: Інтеграція фазингу libFuzzer та підрахунок чистого коду ядра (гейт ≥ 3 500 рядків).
- **M5**: Прогін мутаційного тестування Mull, дотискання тестів до Mutation Score ≥ 80%, фінальний звіт і підготовка до захисту.

---

## 9. Стан реалізації (Implementation Status)

> Розділ додано під час реалізації; розділи 1–8 вище відповідають оригінальному документу.

### 9.1. Виконані етапи: M0, M1, M2

| Етап | Що реалізовано | Код |
|---|---|---|
| M0 | CMake/CTest, Google Test, RapidCheck, опція `LSM_SANITIZE` (ASan/UBSan/TSan), CI на GitHub Actions | `CMakeLists.txt`, `tests/CMakeLists.txt`, `.github/workflows/ci.yml` |
| M1 | Arena, конкурентний SkipList (*p* = 1/4, MaxLevel = 16, двонаправлений ітератор), MemTable з tombstone-ами та політикою flush, WAL writer/reader, Crash Replay | `src/util/`, `src/memtable/`, `src/wal/` |
| M2 | Delta/Varint блоки з restart points, SSTable builder/reader, Bloom filter (Murmur3 + Kirsch-Mitzenmacher), шардований LRU block cache, flush MemTable → L0 SSTable | `src/sstable/`, `src/filter/`, `src/cache/`, `src/db/builder.*` |
| (L5) | Абстракція файлової системи `Env` (реальна ФС + in-memory) для fault injection | `include/lsm/env.h`, `src/env/` |

Не реалізовано (наступні етапи): Compaction Engine і K-way merge (M3), `StorageEngine`/`DBImpl` з фоновими потоками (M3), libFuzzer-цілі (M4), Mull (M5), бенчмарки (L8).

### 9.2. Уточнення бінарних форматів

Оригінальний документ залишає кілька деталей відкритими; реалізація фіксує їх так:

- **Внутрішній ключ.** MemTable і SSTable зберігають `user_key | fixed64(seq << 8 | type)`, де `type` збігається з байтом типу WAL (`0x01` PUT, `0x02` DELETE). Порядок: user key за зростанням, потім sequence за спаданням — найновіша версія знаходиться першою. Bloom filter будується за user key.
- **Трейлер блоку.** Кожен блок SSTable (data, filter, index) супроводжується 4-байтовим CRC32 вмісту. Це дозволяє повертати `Status::Corruption` до розбору пошкодженого блоку (F-01).
- **Footer (48 байтів).** Чотири поля `uint64` з розділу 4.3 (32 байти) + `uint32` версії формату (`1`) + `uint32` CRC32 перших 36 байтів + 8 байтів magic `0xDB47534D5F4B5631`. Filter handle з розміром 0 означає відсутність фільтра.
- **Index Block.** Ключ запису — найбільший ключ data-блоку; значення — `varint64 offset | varint64 size`. Restart interval індексу = 1.
- **Data Block.** Запис: `varint32 shared | varint32 non_shared | varint32 value_len | key_delta | value`; restart interval за замовчуванням 16 (`Options::block_restart_interval`); блок закінчується масивом `fixed32` restart-зміщень і їх кількістю.
- **Bloom filter.** Бітовий масив (мінімум 64 біти) + 1 байт *k*; *k* = round(bits_per_key · ln 2), обмежено [1, 30]. Невідоме *k* трактується як «можливо присутній».
- **WAL Replay.** Запис, обрізаний кінцем файлу (torn write), коректно завершує журнал (`tail_truncated`, статус OK). Помилка CRC, невідомий тип або неправдоподібна довжина (> 64 МБ) дають `Status::Corruption`. В обох випадках усі попередні записи застосовано. Номери послідовності призначаються під час replay послідовно.
- **Flush.** Таблиця пишеться у `NNNNNN.tmp`, синхронізується, перевіряється повторним відкриттям і лише потім перейменовується у `NNNNNN.sst`. При помилці (ENOSPC, EIO) тимчасовий файл видаляється, а MemTable залишається недоторканою (FI-02).

### 9.3. Відповідність тестів каталогу 6.2

| ID | Тест(и) |
|---|---|
| U-01 | `SkipListTest.InsertAndLookup100kRandomKeys`, `BackwardIteration`, `HeightDistributionIsGeometric` |
| U-02 | `BloomFilterTest.NoFalseNegativesAndLowFalsePositiveRate` |
| U-03 | `LRUCacheTest.EvictsLeastRecentlyUsed`, `LookupRefreshesRecency` |
| U-04 | `TableTest.FooterLayoutAndMagicNumber`, `BlockTest.*` |
| U-05 | `WalTest.DetectsCorruptedByteInEveryPosition`, `Crc32Test.*` |
| P-01 | `BlockProperty.P01_EncodeDecodeRoundTrip` |
| P-02 | `MemTableProperty.P02_ReadAfterDelete` |
| P-03 | `IteratorProperty.P03_StrictlyIncreasingKeys` |
| D-01 (рівень MemTable) | `MemTableTest.RandomOperationsMatchStdMap` |
| F-01 (без libFuzzer) | `TableTest.RandomCorruptionNeverCrashes`, `BlockTest.CorruptedEntriesYieldCorruptionNotCrash` |
| F-02 (без libFuzzer) | `WalTest.RandomDamageNeverCrashes` |
| FI-01 | `WalTest.TornWriteRecoversEveryCompleteRecord` |
| FI-02 | `BuilderTest.DiskFullDuringFlushReportsErrorAndKeepsData` |
| S-01/S-03 | повний набір під `-DLSM_SANITIZE=address,undefined` (CI) |
| S-02 (рівень SkipList/кешу/SSTable) | `SkipListTest.ConcurrentReadersWithSingleWriter`, `LRUCacheTest.ShardedCacheUnderConcurrentAccess`, `TableTest.IteratorKeepsTableAliveAndConcurrentReadsWork` під TSan (CI) |
