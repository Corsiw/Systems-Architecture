#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cerrno>

#include <fcntl.h>      // shm_open, open
#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // mode constants
#include <semaphore.h>  // sem_t, sem_open...
#include <unistd.h>     // sleep, getpid
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

using namespace std;

static volatile sig_atomic_t g_terminate = 0;
void sigint_handler(int) { 
    g_terminate = 1;
}

struct Report {
    pid_t group_pid;
    int group_id;
    int section;
    bool found;
    time_t t;
};

struct Shared {
    int next_section;
    int total_sections;
    int reports_prod_idx;
    int reports_cons_idx;
    int processed_reports;
    int buf_size;
    int shutdown;
    int active_workers;
    int max_workers;
    Report reports[1];
};

string base_name = "/treasure_demo";

string get_shm_name() { return base_name + "_shm"; }
string get_shm_name(const string &sfx) { return base_name + sfx; }

// FIFO / observer helpers
const char *FIFO_PATH = "/tmp/treasure_fifo";
static int fifo_fd = -1;

// Игнорируем SIGPIPE, чтобы write() возвращал -1 с errno = EPIPE
void init_fifo_signal_handling() {
    signal(SIGPIPE, SIG_IGN);
}

// Попытка открыть FIFO для записи (non-blocking). Возвращает дескриптор или -1.
int open_fifo_nonblocking() {
    // Убедимся, что FIFO существует; если нет — создадим (возможно, observer создаёт сам)
    struct stat st;
    if (stat(FIFO_PATH, &st) == -1) {
        if (errno == ENOENT) {
            // попытка создать FIFO; если другой процесс одновременно создаст — ok
            if (mkfifo(FIFO_PATH, 0666) == -1) {
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
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        // Если нет читателя, open вернёт -1 с ENXIO — это нормальная ситуация
        return -1;
    }
    return fd;
}

// Надёжная отправка сообщения в наблюдатель. Всегда возвращает true если хотя бы одно действие выполнено:
// - сообщение напечатано в консоль до вызова этой функции (не здесь),
// - функция пытается отправить в FIFO; при ошибке логирует в stderr и закрывает fifo_fd при необходимости.
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

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);

    if(argc < 2 || string(argv[1]) != "open"){
        string usage = string("Usage: ") + argv[0] + " open\n";
        cerr << usage;
        send_to_observer(usage);
        return 1;
    }

    signal(SIGTERM, sigint_handler);
    signal(SIGINT, sigint_handler);
    // Игнорируем SIGPIPE, чтобы write() возвращал -1 на EPIPE
    init_fifo_signal_handling();

    string s_shm = get_shm_name();
    int fd = shm_open(s_shm.c_str(), O_RDWR, 0);
    if(fd == -1){
        string msg = string("[Worker ") + to_string(getpid()) + "] Ошибка shm_open — менеджер не запущен.\n";
        perror("shm_open");
        send_to_observer(msg);
        return 1;
    }

    struct stat st;
    if(fstat(fd, &st) == -1){
        perror("fstat");
        send_to_observer("[Worker] fstat error.\n");
        close(fd);
        return 1;
    }

    size_t shm_sz = st.st_size;
    void* mem = mmap(nullptr, shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(mem == MAP_FAILED){
        perror("mmap");
        send_to_observer("[Worker] mmap failed.\n");
        close(fd);
        return 1;
    }
    close(fd);

    Shared* shared = (Shared*)mem;

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

    if(section_mutex == SEM_FAILED || report_mutex == SEM_FAILED || items_mutex == SEM_FAILED ||
       slots_mutex == SEM_FAILED || workers_mutex == SEM_FAILED){
        perror("sem_open (worker)");
        send_to_observer("[Worker] sem_open failed — проверьте запуск менеджера.\n");
        return 1;
    }

    // Проверка лимита
    sem_wait(workers_mutex);
    if (shared->active_workers >= shared->max_workers) {
        std::ostringstream oss;
        oss << "[Worker pid=" << getpid() << "] Максимальное число активных групп ("
            << shared->max_workers << ") уже достигнуто. Завершение.\n";
        send_to_observer(oss.str());
        sem_post(workers_mutex);
        return 0;
    }
    shared->active_workers++;
    sem_post(workers_mutex);

    int group_id = (int)(getpid() % 10000);
    srand((unsigned)time(nullptr) ^ getpid());

    {
        std::ostringstream start;
        start << "[Worker pid=" << getpid() << "] Запущен. Начинаю поиск.\n";
        cout << start.str();
        send_to_observer(start.str());
    }

    while(!g_terminate) {
        if(shared->shutdown) {
            {
                std::ostringstream oss;
                oss << "[Worker pid=" << getpid() << "] замечен shutdown флаг — завершаюсь.\n";
                cout << oss.str();
                send_to_observer(oss.str());
            }
            break;
        }

        if(sem_wait(section_mutex) == -1) {
            if(errno == EINTR) continue;
            perror("sem_wait mutex (worker)");
            send_to_observer("[Worker] Ошибка sem_wait mutex.\n");
            break;
        }

        int section = shared->next_section;
        if(section >= shared->total_sections) {
            sem_post(section_mutex);
            {
                std::ostringstream oss;
                oss << "[Worker pid=" << getpid() << "] участков больше нет — завершаюсь.\n";
                cout << oss.str();
                send_to_observer(oss.str());
            }
            break;
        }
        shared->next_section++;
        sem_post(section_mutex);

        int work = 1 + rand() % 3;
        {
            std::ostringstream msg;
            msg << "[Worker pid=" << getpid() << "] берёт участок #" << section << ", ищет " << work << "s\n";
            cout << msg.str();
            send_to_observer(msg.str());
        }

        sleep(work);
        bool found = (rand() % 100) < 10;

        if(sem_wait(slots_mutex) == -1){
            if(errno == EINTR) continue;
            perror("sem_wait slots (worker)");
            send_to_observer("[Worker] Ошибка sem_wait slots.\n");
            break;
        }
        if(sem_wait(report_mutex) == -1){
            perror("sem_wait report_mutex (worker)");
            send_to_observer("[Worker] Ошибка sem_wait report_mutex.\n");
            sem_post(slots_mutex);
            break;
        }

        int idx = shared->reports_prod_idx % shared->buf_size;
        Report* rep = &shared->reports[idx];
        rep->group_pid = getpid();
        rep->group_id = group_id;
        rep->section = section;
        rep->found = found;
        rep->t = time(nullptr);
        shared->reports_prod_idx++;

        sem_post(report_mutex);
        sem_post(items_mutex);


        {
            std::ostringstream report;
            report << "[Worker pid=" << getpid() << "] отправил отчёт по участку #" << section
                << (found ? " (НАШЁЛ!)" : " (ничего)") << "\n";
            cout << report.str();
            send_to_observer(report.str());
        }

        sleep(rand() % 2);
    }

    sem_close(section_mutex);
    sem_close(report_mutex);
    sem_close(items_mutex);
    sem_close(slots_mutex);
    sem_close(workers_mutex);
    munmap(mem, shm_sz);

    {
        std::ostringstream oss;
        oss << "[Worker pid=" << getpid() << "] завершился корректно.\n";
        cout << oss.str();
        send_to_observer(oss.str());
    }
    return 0;
}
