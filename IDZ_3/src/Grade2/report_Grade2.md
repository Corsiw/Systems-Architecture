# Grade2. Отечет на оценку 7-8. Что изменилось по сравнению с 4-6?

## **1. Сценарий задачи**

1. **Джон Сильвер (управляющий процесс)**

   * создаёт именованную разделяемую память и набор именованных семафоров;
   * ждёт отчётов от пиратских групп;
   * завершает работу после получения всех отчётов.

2. **Группы пиратов (дочерние процессы, запускаемые отдельно)**

   * подключаются к уже существующим объектам IPC (shared memory и семафоры);
   * проверяют, разрешено ли подключение новых групп (через семафор /worker_mutex и счётчик active_workers);
   * получают номер участка для поиска;
   * «ищут клад» (имитация задержкой `sleep()`);
   * записывают отчёт в разделяемую память;
   * уведомляют Сильвера семафором.

3. Каждый процесс запускается **в отдельной консоли**, выводя свой собственный ход работы.

---

## **2. Взаимодействие процессов**

Механизм синхронизации основан на **именованных POSIX семафорах**:

| Название семафора     | Назначение                                                                   |
| --------------------- | ---------------------------------------------------------------------------- |
| `_mutex`              | обеспечивает взаимное исключение при выдаче следующего участка               |
| `_report`             | защита буфера отчётов при записи и чтении                                    |
| `_items`              | количество доступных отчётов для чтения                                      |
| `_slots`              | количество свободных слотов для новых отчётов                                |
| `_workers`            | защита и управление счётчиками, чтобы ограничить число одновременных пиратов |

Передача данных осуществляется через **именованную разделяемую память** (`/treasure_shm`), в которой содержатся:

* индексы для циклического буфера,
* количество обработанных отчётов,
* массив структур `Report` с данными об участках и результатах поиска.

---

## **3. Реализация**

### Общая структура

Проект состоит из двух исполняемых файлов:

* **manager_named.cpp** — программа Джона Сильвера (управляющий процесс);
* **worker_named.cpp** — программа одной пиратской группы.

---

### **3.1 [`manager_named.cppp`](manager_named.cpp) (управляющий процесс)**

Ключевые шаги:

```cpp
    Shared* shared = (Shared*)mem;
    // Инициализация управл. полей
    shared->next_section = 0;
    shared->total_sections = num_sections;
    shared->reports_prod_idx = 0;
    shared->reports_cons_idx = 0;
    shared->processed_reports = 0;
    shared->buf_size = buf_size;
    shared->shutdown = 0;
    shared->active_workers = 0;
    shared->max_workers = num_groups;

    ...

    // named semaphores
    string s_mutex = get_shm_name("_mutex");
    string s_report = get_shm_name("_report");
    string s_items = get_shm_name("_items");
    string s_slots = get_shm_name("_slots");
    string s_workers = get_shm_name("_workers");

    // Удаляем существующие semaphores, если создались с крашем
    safe_sem_unlink(s_mutex.c_str());
    safe_sem_unlink(s_report.c_str());
    safe_sem_unlink(s_items.c_str());
    safe_sem_unlink(s_slots.c_str());
    safe_sem_unlink(s_workers.c_str());

    // Инициализируем семафоры
    sem_t* section_mutex = sem_open(s_mutex.c_str(), O_CREAT | O_EXCL, 0600, 1);
    sem_t* report_mutex = sem_open(s_report.c_str(), O_CREAT | O_EXCL, 0600, 1);
    sem_t* items_mutex = sem_open(s_items.c_str(), O_CREAT | O_EXCL, 0600, 0);
    sem_t* slots_mutex = sem_open(s_slots.c_str(), O_CREAT | O_EXCL, 0600, buf_size);
    sem_t* workers_mutex = sem_open(s_workerss.c_str(), O_CREAT | O_EXCL, 0600, 1);

    // Приём отчётов
    while (shared->processed_reports < total_sections) {
        ...
    }

    // Очистка: уничтожение семафоров и shared memory
    sem_close(section_mutex);
    sem_close(report_mutex);
    sem_close(items_mutex);
    sem_close(slots_mutex);
    sem_close(workers_mutex);
    // unlink именованных семафоров
    safe_sem_unlink(s_mutex.c_str());
    safe_sem_unlink(s_report.c_str());
    safe_sem_unlink(s_items.c_str());
    safe_sem_unlink(s_slots.c_str());
    safe_sem_unlink(s_workers.c_str());

    munmap(mem, shm_size);
    shm_unlink(shm_name.c_str());
```

---

### **3.2 [`worker_named.cppp`](worker_named.cpp) (пиратская группа)**

Каждый пират подключается к тем же IPC-объектам и выполняет поиск участка:

```cpp
    // Подключение к разделяемой памяти
    void* mem = mmap(nullptr, shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Shared* shared = (Shared*)mem;

    // named semaphores
    string s_mutex = get_shm_name("_mutex");
    string s_report = get_shm_name("_report");
    string s_items = get_shm_name("_items");
    string s_slots = get_shm_name("_slots");
    string s_workers = get_shm_name("_workers");

    sem_t* section_mutex = sem_open(s_mutex.c_str(), 0);
    sem_t* report_mutex = sem_open(s_report.c_str(), 0);
    sem_t* items_mutex = sem_open(s_items.c_str(), 0);
    sem_t* slots_mutex = sem_open(s_slots.c_str(), 0);
    sem_t* workers_mutex = sem_open(s_workers.c_str(), 0);


    // Отправка отчетов
    while (true) {
        ...
    }

    // Закрытие семафоров
    sem_close(section_mutex);
    sem_close(report_mutex);
    sem_close(items_mutex);
    sem_close(slots_mutex);
    sem_close(workers_mutex);

    munmap(mem, shm_sz);
```

---

## **4. Завершение работы и очистка**

* Все именованные объекты удаляются вызовами `sem_unlink()` и `shm_unlink()` в процессе Сильвера.
* При нажатии **Ctrl+C** предусмотрена корректная обработка сигналов (через `sigaction`) — все объекты удаляются, а процессы завершаются.
* Повторный запуск возможен после завершения предыдущего экземпляра.

---

## **5. Запуск**

1. Компиляция:

    ```bash
    g++ -std=c++17 -pthread -O2 -o manager_named src/Grade2/manager_named.cpp

    g++ -std=c++17 -pthread -O2 -o worker_named src/Grade2/worker_named.cpp
    ```

2. Сначала запустить **управляющий процесс**:

   ```bash
   ./manager_named 2 10 10
   ```

3. Затем — несколько независимых пиратских процессов:

   ```bash
   ./worker_named open
   ./worker_named open
   ./worker_named open
   ```

4. Каждый пират работает в **своём окне консоли** и пишет туда лог своей деятельности.
   Сильвер в своём окне принимает и выводит отчёты.

---

## **6. Результаты работы**

* Каждый пират последовательно получает участок и ищет клад.
* Сильвер в реальном времени принимает отчёты.
* После того как все участки обработаны, программа корректно завершает работу и удаляет IPC-объекты.

Пример вывода в консолях:

**Консоль Сильвера:**

```
Manager(pid=24136): создана SHM и семафоры.
SHM name: /treasure_demo_shm
Semaphores: /treasure_demo_mutex, /treasure_demo_report, /treasure_demo_items, /treasure_demo_slots, 
Ожидайте запуска рабочих в других консолях командой: ./worker_named open
[Silver] Получен отчёт: группа 4193 (pid=24193) участок #0 => пусто  time=2025-11-11 00:08:06
[Silver] Получен отчёт: группа 4193 (pid=24193) участок #1 => пусто  time=2025-11-11 00:08:08
[Silver] Получен отчёт: группа 4263 (pid=24263) участок #2 => пусто  time=2025-11-11 00:08:11
[Silver] Получен отчёт: группа 4193 (pid=24193) участок #3 => пусто  time=2025-11-11 00:08:11
[Silver] Получен отчёт: группа 4263 (pid=24263) участок #4 => пусто  time=2025-11-11 00:08:13
[Silver] Получен отчёт: группа 4193 (pid=24193) участок #5 => пусто  time=2025-11-11 00:08:14
[Silver] Получен отчёт: группа 4263 (pid=24263) участок #6 => пусто  time=2025-11-11 00:08:15
[Silver] Получен отчёт: группа 4443 (pid=24443) участок #7 => пусто  time=2025-11-11 00:08:16
[Silver] Получен отчёт: группа 4193 (pid=24193) участок #8 => Сундук НАЙДЕН!  time=2025-11-11 00:08:17
[Silver] Получен отчёт: группа 4443 (pid=24443) участок #9 => пусто  time=2025-11-11 00:08:19
Manager: обработано отчётов: 10 из 10
[Silver] Семафоры и разделяемая память удалены. Программа завершена.
```

**Консоль пирата 1:**

```
[Worker pid=24193] берёт участок #0, время 3s
[Worker pid=24193] отправил отчёт по участку #0 (ничего)
[Worker pid=24193] берёт участок #1, время 2s
[Worker pid=24193] отправил отчёт по участку #1 (ничего)
[Worker pid=24193] берёт участок #3, время 2s
[Worker pid=24193] отправил отчёт по участку #3 (ничего)
[Worker pid=24193] берёт участок #5, время 2s
[Worker pid=24193] отправил отчёт по участку #5 (ничего)
[Worker pid=24193] берёт участок #8, время 2s
[Worker pid=24193] отправил отчёт по участку #8 (НАШЁЛ!)
[Worker pid=24193] участков больше нет — завершаюсь.
[Worker pid=24193] завершился корректно.
```

---
