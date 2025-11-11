# Grade3. Отечет на оценку 9. Что изменилось по сравнению с 7-8?

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
| Именованный канал (FIFO)       | Все процессы     | Отправка сообщений наблюдателю                     |

---

## **4. Реализация**

### 4.1 [`observer.cpp`](observer.cpp)

Создаёт FIFO (если не существует), открывает его на чтение и печатает всё, что туда приходит.

```cpp
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <csignal>

volatile sig_atomic_t running = 1;

void handle_sigint(int) {
    running = 0;
    std::cout << "\n[Observer] Завершение по Ctrl+C\n";
}

int main() {
    const char *fifo_path = "/tmp/treasure_fifo";
    signal(SIGINT, handle_sigint);

    mkfifo(fifo_path, 0666); // создать, если нет

    int fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open fifo");
        return 1;
    }

    std::cout << "[Observer] Наблюдатель запущен. Ожидание сообщений...\n";

    char buf[256];
    while (running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << buf;
            std::cout.flush();
        } else {
            usleep(200000); // небольшая пауза
        }
    }

    close(fd);
    unlink(fifo_path);
    std::cout << "[Observer] FIFO закрыт и удалён.\n";
}
```

---

### 4.2 Изменения в [`manager_named.cpp`](manager_named.cpp) и [`worker_named.cpp`](worker_named.cpp)

Добавил метод отправки сообщений наблюдателю и сделал его вызов в местах вывода сообщений:

```cpp
void send_to_observer(const std::string &msg) {
    // Если FIFO дескриптор не открыт, попробуем открыть
    if (fifo_fd == -1) {
        fifo_fd = open_fifo_nonblocking();
        if (fifo_fd == -1) {
            // Нет доступного FIFO/читателя — логируем в stderr (и остаёмся работать)
            std::cerr << "[Manager][WARN] FIFO not available for observer; message not sent: "
                      << (msg.size() > 200 ? msg.substr(0,200) + "..." : msg) ;
            // ensure newline
            if (msg.empty() || msg.back() != '\n') std::cerr << "\n";
            return;
        }
    }

    // Пишем весь буфер (учтём возможные частичные записи)
    const char* data = msg.c_str();
    size_t remaining = msg.size();
    while (remaining > 0) {
        ssize_t w = write(fifo_fd, data, remaining);
        if (w > 0) {
            data += w;
            remaining -= (size_t)w;
            continue;
        }
        // w <= 0 => ошибка
        if (w == -1) {
            if (errno == EINTR) {
                // прервано сигналом — попробуем снова
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // FIFO временно недоступен (буфер полон) — не блокируем manager, логируем и отбрасываем сообщение
                std::cerr << "[Manager][WARN] FIFO write would block, message dropped: "
                          << (msg.size() > 200 ? msg.substr(0,200) + "..." : msg);
                if (msg.empty() || msg.back() != '\n') std::cerr << "\n";
                return;
            } else if (errno == EPIPE) {
                // Читатель закрыл канал — закроем дескриптор и пометим как недоступный
                std::cerr << "[Manager][WARN] FIFO broken (EPIPE). Closing fifo_fd and will retry later.\n";
                close(fifo_fd);
                fifo_fd = -1;
                return;
            } else {
                // Прочие ошибки — логируем и закрываем дескриптор
                std::cerr << "[Manager][ERROR] write to FIFO failed: " << strerror(errno) << "\n";
                close(fifo_fd);
                fifo_fd = -1;
                return;
            }
        } else {
            // w == 0 — возможно некорректная ситуация, закроем и выйдем
            std::cerr << "[Manager][WARN] write returned 0, closing fifo_fd\n";
            close(fifo_fd);
            fifo_fd = -1;
            return;
        }
    }
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
    g++ -std=c++17 -pthread -O2 -o manager_named src/Grade3/manager_named.cpp

    g++ -std=c++17 -pthread -O2 -o worker_named src/Grade3/worker_named.cpp

    g++ -std=c++17 -pthread -O2 -o observer src/Grade3/observer.cpp
    ```

2. Запустить **observer**:
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