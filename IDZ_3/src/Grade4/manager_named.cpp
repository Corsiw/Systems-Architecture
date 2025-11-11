#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cerrno>

#include <fcntl.h>      // shm_open, O_*
#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // ftruncate, mode constants, mkfifo
#include <semaphore.h>  // sem_t, sem_open...
#include <unistd.h>     // close, write, sleep, getpid
#include <sys/wait.h>   // waitpid
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

using namespace std;

static volatile sig_atomic_t g_stop = 0;
void sigint_handler(int) {
    g_stop = 1;
}

struct Report {
    pid_t group_pid;
    int group_id;
    int section;
    bool found;
    time_t t;
};

struct Shared {
    // управляющие поля
    int next_section;
    int total_sections;
    int reports_prod_idx;
    int reports_cons_idx;
    int processed_reports;
    int buf_size;
    int shutdown;
    int active_workers;
    int max_workers;
    // flexible array of reports
    Report reports[1];
};

size_t shmsize_for(int buf_size) {
    return sizeof(Shared) + (size_t)(buf_size - 1) * sizeof(Report);
}

string base_name = "/treasure_demo";

string get_shm_name() {
    return base_name + "_shm";
}

string get_sem_name(const string &sfx) {
    return base_name + sfx;
}

// helper, чтобы анлинкнуть старые семафоры (игнорируем ошибки)
void safe_sem_unlink(const char* name){
    if (name == nullptr) return;
    sem_unlink(name);
}

// FIFO / observer helpers
// const char *FIFO_PATH = "/tmp/treasure_observer_fifo";

// Игнорируем SIGPIPE, чтобы write() возвращал -1 с errno = EPIPE
void init_fifo_signal_handling() {
    signal(SIGPIPE, SIG_IGN);
}

// Попытка открыть FIFO для записи (non-blocking). Возвращает дескриптор или -1.
int open_fifo_nonblocking(const std::string& path) {
    // Убедимся, что FIFO существует; если нет — создадим (возможно, observer создаёт сам)
    struct stat st;
    if (stat(path.c_str(), &st) == -1) {
        if (errno == ENOENT) {
            // попытка создать FIFO; если другой процесс одновременно создаст — ok
            if (mkfifo(path.c_str(), 0666) == -1) {
                // если не удалось создать, это не фатально — просто сообщим и вернём -1
                // (может создать observer)
                // Не используем perror здесь, потому что мы будем логгировать через send_to_observer wrapper
                return -1;
            }
        } else {
            return -1;
        }
    }

    // Открываем для записи в неблокирующем режиме
    int fd = open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        // Если нет читателя, open вернёт -1 с ENXIO — это нормальная ситуация
        return -1;
    }
    return fd;
}

void send_to_observer(const std::string& msg, const std::string& path) {
    // Если FIFO дескриптор не открыт, попробуем открыть
    int fifo_fd = open_fifo_nonblocking(path);
    if (fifo_fd == -1) {
        // Нет доступного FIFO/читателя — логируем в stderr (и остаёмся работать)
        std::cerr << "[Manager][WARN] FIFO not available for observer; message not sent: "
                    << (msg.size() > 200 ? msg.substr(0,200) + "..." : msg) ;
        // ensure newline
        if (msg.empty() || msg.back() != '\n') std::cerr << "\n";
        return;
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

// Надёжная отправка сообщения в наблюдатель. Всегда возвращает true если хотя бы одно действие выполнено:
// - сообщение напечатано в консоль до вызова этой функции (не здесь),
// - функция пытается отправить в FIFO; при ошибке логирует в stderr и закрывает fifo_fd при необходимости.
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

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <num_groups> <num_sections> [report_buffer_size]\n";
        return 1;
    }

    int num_groups = stoi(argv[1]);
    int num_sections = stoi(argv[2]);
    int buf_size = 128;
    if (argc >= 4) buf_size = stoi(argv[3]);
    if (num_groups <= 0 || num_sections <= 0 || buf_size <= 0) {
        cerr << "Arguments must be positive integers.\n";
        return 1;
    }

    if (num_sections <= num_groups) {
        cerr << "По условию число участков (" << argv[2] << ") должно превышать число групп (" << argv[1] << ").\n";
        return 1;
    }

    // Обработчик SIGINT
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // Игнорируем SIGPIPE, чтобы write() возвращал -1 на EPIPE
    init_fifo_signal_handling();

    string shm_name = get_shm_name();
    size_t shm_size = shmsize_for(buf_size);

    // Создаём POSIX shared memory
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open");
        // также отправим в observer (если возможно)
        std::ostringstream eoss;
        eoss << "[Manager][ERROR] shm_open failed: " << strerror(errno) << "\n";
        send_to_observers(eoss.str());
        return 1;
    }

    if (ftruncate(fd, (off_t)shm_size) == -1) {
        perror("ftruncate");
        shm_unlink(shm_name.c_str());
        std::ostringstream eoss;
        eoss << "[Manager][ERROR] ftruncate failed: " << strerror(errno) << "\n";
        send_to_observers(eoss.str());
        return 1;
    }

    void* mem = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        shm_unlink(shm_name.c_str());
        std::ostringstream eoss;
        eoss << "[Manager][ERROR] mmap failed: " << strerror(errno) << "\n";
        send_to_observers(eoss.str());
        return 1;
    }
    close(fd); // дескриптор можно закрыть, область останется доступной через mmap

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

    // named semaphores
    string s_mutex = get_sem_name("_mutex");
    string s_report = get_sem_name("_report");
    string s_items = get_sem_name("_items");
    string s_slots = get_sem_name("_slots");
    string s_workers = get_sem_name("_workers");

    // Удаляем существующие semaphores, если остались от краша
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
    sem_t* workers_mutex = sem_open(s_workers.c_str(), O_CREAT | O_EXCL, 0600, 1);

    if (section_mutex == SEM_FAILED || report_mutex == SEM_FAILED || items_mutex == SEM_FAILED ||
        slots_mutex == SEM_FAILED || workers_mutex == SEM_FAILED) {
        perror("sem_open (create)");
        std::ostringstream eoss;
        eoss << "[Manager][ERROR] sem_open failed: " << strerror(errno) << "\n";
        send_to_observers(eoss.str());

        if (section_mutex != SEM_FAILED) sem_close(section_mutex);
        if (report_mutex != SEM_FAILED) sem_close(report_mutex);
        if (items_mutex != SEM_FAILED) sem_close(items_mutex);
        if (slots_mutex != SEM_FAILED) sem_close(slots_mutex);
        if (workers_mutex != SEM_FAILED) sem_close(workers_mutex);
        shm_unlink(shm_name.c_str());
        return 1;
    }

    // Информационные сообщения — печатаем и отправляем в observer
    {
        std::ostringstream oss;
        oss << "[Manager](pid=" << getpid() << "): создана SHM и семафоры.\n";
        cout << oss.str();
        send_to_observers(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "SHM name: " << shm_name << "\n";
        cout << oss.str();
        send_to_observers(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Semaphores: " << s_mutex << ", " << s_report << ", " << s_items << ", " << s_slots << ", " << s_workers << "\n";
        cout << oss.str();
        send_to_observers(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Ожидайте запуска рабочих в других консолях командой: ./worker_named open\n";
        cout << oss.str();
        send_to_observers(oss.str());
    }

    // Сильвер — принимает отчёты
    int total_to_process = num_sections;
    while (!g_stop && shared->processed_reports < total_to_process) {
        // ждём появления элемента
        if (sem_wait(items_mutex) == -1) {
            if (errno == EINTR) {
                if (g_stop) break;
                continue;
            }
            perror("sem_wait items");
            std::ostringstream eoss;
            eoss << "[Manager][ERROR] sem_wait(items) failed: " << strerror(errno) << "\n";
            send_to_observers(eoss.str());
            break;
        }

        if (sem_wait(report_mutex) == -1) {
            perror("sem_wait report_mutex");
            std::ostringstream eoss;
            eoss << "[Manager][ERROR] sem_wait(report_mutex) failed: " << strerror(errno) << "\n";
            send_to_observers(eoss.str());
            sem_post(items_mutex); // попытка сохранить целостность
            break;
        }

        int idx = shared->reports_cons_idx % shared->buf_size;
        Report rep = shared->reports[idx]; // копируем наружу
        shared->reports_cons_idx++;
        shared->processed_reports++;

        sem_post(report_mutex);
        sem_post(slots_mutex);

        // Обработка отчёта — формируем сообщение
        char tbuf[64];
        struct tm tm;
        localtime_r(&rep.t, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

        std::ostringstream oss;
        oss << "[Manager] Получен отчёт: группа " << rep.group_id
            << " (pid=" << rep.group_pid << ") участок #" << rep.section
            << (rep.found ? " => Сундук НАЙДЕН!" : " => пусто")
            << "  time=" << tbuf << "\n";

        // Печатаем в консоль и отправляем в observer
        std::string msg = oss.str();
        cout << msg;
        send_to_observers(msg);
    }

    // Если прервано клавишей — оповещаем worker процессы
    if (g_stop) {
        // выставляем флаг shutdown, чтобы все рабочие корректно завершились
        std::ostringstream oss;
        oss << "[Manager] SIGINT получен — выставляю shutdown флаг и ожидаю завершения рабочих...\n";
        cout << oss.str();
        send_to_observers(oss.str());
        shared->shutdown = 1;
    }

    sleep(2);

    {
        std::ostringstream oss;
        oss << "[Manager] обработано отчётов: " << shared->processed_reports << " из " << total_to_process << "\n";
        cout << oss.str();
        send_to_observers(oss.str());
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

    {
        std::ostringstream oss;
        oss << "[Manager] Семафоры и разделяемая память удалены. Программа завершена.\n";
        cout << oss.str();
        // Попытка отправить финальное сообщение (если FIFO доступен)
        send_to_observers(oss.str());
    }

    return 0;
}
