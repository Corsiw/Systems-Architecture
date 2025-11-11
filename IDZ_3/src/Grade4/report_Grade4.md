# Grade3. Отечет на оценку 10. Что изменилось по сравнению с 9?

## **1. Сценарий задачи**

Теперь в системе участвуют три типа процессов:

1. **Джон Сильвер (`manager_named`)**

   * координирует группы и обрабатывает отчёты;
   * выводит собственную информацию в свою консоль;
   * передаёт ключевые сообщения наблюдателю через именованный канал (FIFO).

2. **Группы пиратов (`worker_named`)**

   * ищут участки;
   * печатают результаты в свою консоль;
   * отправляют уведомления о действиях наблюдателю.

3. **Наблюдатель (`observer`)**

   * получает сообщения из именованного канала `/treasure_fifo`;
   * отображает в своей консоли сводку всех событий в реальном времени.

---

## **2. Средства взаимодействия**

| Механизм                       | Используется кем | Назначение                                         |
| ------------------------------ | ---------------- | -------------------------------------------------- |
| Именованная разделяемая память | Менеджер, пираты | Хранение отчётов, данных об участках               |
| Именованные POSIX семафоры     | Менеджер, пираты | Синхронизация (доступ к участкам, буферу, отчётам) |
| Именованные канал (FIFO)       | Все процессы     | Отправка сообщений наблюдателю                     |

---

## **4. Реализация**

### 4.1 [`observer.cpp`](observer.cpp)

Создаёт уникальный FIFO, завясящий от процесса (если не существует), открывает его на чтение и печатает всё, что туда приходит.

```cpp
#include <iostream>
#include <string>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

using namespace std;

static volatile sig_atomic_t g_stop = 0;
void sigint_handler(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Уникальное имя FIFO
    pid_t pid = getpid();
    string fifo_name = "/tmp/treasure_observer_fifo_" + to_string(pid);

    // Создание FIFO
    if (mkfifo(fifo_name.c_str(), 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo");
            return 1;
        }
    }

    cout << "[Observer pid=" << pid << "] создан канал: " << fifo_name << endl;
    cout << "Ожидаю сообщения...\n";

    int fd = open(fifo_name.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open fifo");
        unlink(fifo_name.c_str());
        return 1;
    }

    char buf[512];
    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << buf;
        } else {
            usleep(200000); // небольшая пауза
        }
    }

    cout << "\n[Observer pid=" << pid << "] Завершаюсь...\n";
    close(fd);
    unlink(fifo_name.c_str());
    return 0;
}
```

---

### 4.2 Изменения в [`manager_named.cpp`](manager_named.cpp) и [`worker_named.cpp`](worker_named.cpp)

Добавил метод отправки сообщений всем наблюдателям send_to_observers, сделал его вызов в местах вывода сообщений, изменил сигнатуру других методов работы с FIFO, чтобы подходили для работы с FIFO по переданному пути. Метод send_to_observers перебирает все FIFO с определенной сигнатурой в папке /tmp:

```cpp
void send_to_observers(const std::string &msg) {
    const char* dir = "/tmp";
    DIR* dp = opendir(dir);
    if (!dp) return;

    struct dirent* ent;
    while ((ent = readdir(dp)) != nullptr) {
        std::string name = ent->d_name;
        if (name.find("treasure_observer_fifo_") == 0) {
            std::string path = std::string("/tmp/") + name;
            send_to_observer(msg, path);
        }
    }
    closedir(dp);
}
```
---

## **5. Завершение и очистка**

* Все процессы завершаются корректно при `Ctrl+C` (через `sigaction`).
* FIFO удаляется наблюдателем при завершении.
* Семафоры и shared memory удаляются менеджером.

---

## **6. Запуск**

1. Компиляция:

    ```bash
    g++ -std=c++17 -pthread -O2 -o manager_named src/Grade4/manager_named.cpp

    g++ -std=c++17 -pthread -O2 -o worker_named src/Grade4/worker_named.cpp

    g++ -std=c++17 -pthread -O2 -o observer src/Grade4/observer.cpp
    ```

2. Запустить **observer** или несколько:
    ```bash
    ./observer
    ```

3. Запустить **управляющий процесс**:

   ```bash
   ./manager_named 2 10 10
   ```

4. Затем — несколько независимых пиратских процессов:

   ```bash
   ./worker_named open
   ./worker_named open
   ./worker_named open
   ```

4. Каждый пират работает в **своём окне консоли** и пишет туда лог своей деятельности.
   Сильвер в своём окне принимает и выводит отчёты.

---

## **7. Пример вывода**

**Консоль наблюдателя:**

```
[Observer] Наблюдатель запущен. Ожидание сообщений...
[Worker pid=48592] Запущен. Начинаю поиск.
[Worker pid=48592] берёт участок #0, ищет 2s
[Worker pid=48592] отправил отчёт по участку #0 (ничего)
[Manager] Получен отчёт: группа 8592 (pid=48592) участок #0 => пусто  time=2025-11-11 02:14:08
...
```