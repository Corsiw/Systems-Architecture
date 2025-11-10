#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cerrno>

#include <fcntl.h>      // shm_open
#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // mode constants
#include <semaphore.h>  // sem_t, sem_init...
#include <unistd.h>     // fork, sleep, getpid
#include <sys/wait.h>   // waitpid
#include <signal.h>

using namespace std;

volatile sig_atomic_t parent_stop = 0;
void parent_sigint_handler(int) {
    parent_stop = 1;
}

volatile sig_atomic_t child_stop = 0;
void child_sigterm_handler(int) {
    child_stop = 1;
}

struct Report {
    pid_t group_pid;
    int group_id;
    int section;
    bool found;
    time_t t;
};

struct Shared {
    // семафоры (неименованные POSIX), должны быть инициализированы с pshared = 1
    sem_t mutex_next_section; // защита next_section
    sem_t report_mutex;       // защита индексов прод/конс отчётного буфера
    sem_t items;              // заполненные слоты в буфере отчётов
    sem_t slots;              // свободные слоты
    sem_t print_mutex;
    // управляющие поля
    int next_section;
    int total_sections;
    int reports_prod_idx;
    int reports_cons_idx;
    int processed_reports;
    int buf_size;
    // буфер фиксированного размера (будет использоваться как flexible array)
    // но здесь укажем один элемент; фактический размер учтём при mmap.
    Report reports[1];
};

size_t shmsize_for(int buf_size) {
    return sizeof(Shared) + (size_t)(buf_size - 1) * sizeof(Report);
}

string shm_name_from_pid() {
    // имя разделяемой памяти уникально для запуска
    char buf[64];
    snprintf(buf, sizeof(buf), "/treasure_shm_%d", getpid());
    return string(buf);
}

void cleanup_shared_region(const string &name, Shared* ptr, size_t size) {
    // Попытаться уничтожить семафоры (если ptr валиден)
    if (ptr) {
        // Дестрой семафоры — только если это родитель, который инициализировал
        sem_destroy(&ptr->mutex_next_section);
        sem_destroy(&ptr->report_mutex);
        sem_destroy(&ptr->items);
        sem_destroy(&ptr->slots);
        sem_destroy(&ptr->print_mutex);
        munmap(ptr, size);
    }
    // Удаляем объект разделяемой памяти
    shm_unlink(name.c_str());
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc < 4) {
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

    string shm_name = shm_name_from_pid();
    size_t shm_size = shmsize_for(buf_size);

    // Создаём POSIX shared memory
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(fd, (off_t)shm_size) == -1) {
        perror("ftruncate");
        shm_unlink(shm_name.c_str());
        return 1;
    }

    void* mem = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        shm_unlink(shm_name.c_str());
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

    // Инициализируем неименованные POSIX семафоры в разделяемой памяти
    if (sem_init(&shared->mutex_next_section, 1, 1) == -1 ||
        sem_init(&shared->report_mutex, 1, 1) == -1 ||
        sem_init(&shared->items, 1, 0) == -1 ||
        sem_init(&shared->slots, 1, buf_size) == -1 ||
        sem_init(&shared->print_mutex, 1, 1) == -1)
    {
        perror("sem_init");
        cleanup_shared_region(shm_name, shared, shm_size);
        return 1;
    }

    // Установим обработчик SIGINT для родителя
    struct sigaction sa{};
    sa.sa_handler = parent_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    cout << "Silver(pid=" << getpid() << "): запущен. Групп: " << num_groups
         << ", Участков: " << num_sections << ", Буфер отчётов: " << buf_size << ".\n";

    // Массив дочерних pid
    vector<pid_t> children;
    children.reserve(num_groups);

    // Функция для форка дочернего процесса (группа)
    auto fork_group = [&](int group_id) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            // --- дочерний процесс (группа) ---
            setvbuf(stdout, nullptr, _IONBF, 0);
            std::cout.setf(std::ios::unitbuf);

            // обработчик SIGTERM для корректного завершения
            struct sigaction sc{};
            sc.sa_handler = child_sigterm_handler;
            sigemptyset(&sc.sa_mask);
            sc.sa_flags = 0;
            sigaction(SIGTERM, &sc, nullptr);

            srand((unsigned)(time(nullptr) ^ getpid()));

            while (!child_stop) {
                // Берём следующий участок
                if (sem_wait(&shared->mutex_next_section) == -1) {
                    if (errno == EINTR) continue;
                    perror("sem_wait mutex_next_section (child)");
                    break;
                }
                int section = shared->next_section;
                if (section >= shared->total_sections) {
                    // больше участков — освобождаем mutex и выходим
                    sem_post(&shared->mutex_next_section);
                    break;
                }
                shared->next_section++;
                sem_post(&shared->mutex_next_section);

                // Симуляция поиска
                int work = 1 + rand() % 3; // 1..3 секунд
                pid_t mypid = getpid();

                sem_wait(&shared->print_mutex);
                cout << "[Group " << group_id << " pid=" << mypid << "] берёт участок #" << section
                     << ", время поиска " << work << "s\n";
                sem_post(&shared->print_mutex);
                sleep(work);

                bool found = ( (rand() % 100) < 10 ); // например 10% шанс найти клад
                // Сформировать отчет и положить в буфер
                if (sem_wait(&shared->slots) == -1) {
                    if (errno == EINTR) continue;
                    perror("sem_wait slots (child)");
                    break;
                }
                if (sem_wait(&shared->report_mutex) == -1) {
                    perror("sem_wait report_mutex (child)");
                    sem_post(&shared->slots);
                    break;
                }

                int idx = shared->reports_prod_idx % shared->buf_size;
                Report* rep = &shared->reports[idx];
                rep->group_pid = mypid;
                rep->group_id = group_id;
                rep->section = section;
                rep->found = found;
                rep->t = time(nullptr);
                shared->reports_prod_idx++;

                sem_post(&shared->report_mutex);
                sem_post(&shared->items);

                sem_wait(&shared->print_mutex);
                cout << "[Group " << group_id << " pid=" << mypid << "] отправил отчёт по участку #"
                     << section << (found ? " (НАШЁЛ!)" : " (ничего)") << "\n";
                sem_post(&shared->print_mutex);
                
                // небольшая пауза перед взятием следующего участка
                sleep( (rand() % 2) ); // 0..1 s
            }

            // дочерний процесс заканчивает
            sem_wait(&shared->print_mutex);
            cout << "[Group " << group_id << " pid=" << getpid() << "] завершает работу.\n";
            sem_post(&shared->print_mutex);
            _exit(0);
        } else {
            // в родителе
            return pid;
        }
    };

    // Форкаем дочерние процессы
    for (int i = 0; i < num_groups; ++i) {
        pid_t c = fork_group(i+1);
        if (c > 0) children.push_back(c);
        else {
            // ошибка форка — продолжим форкать остальные (или можно выйти)
            cerr << "Не удалось создать дочерний процесс для группы " << (i+1) << "\n";
        }
    }

    // Родитель (Сильвер) — принимает отчёты
    int total_to_process = num_sections;
    while (!parent_stop && shared->processed_reports < total_to_process) {
        // ждём появления элемента
        if (sem_wait(&shared->items) == -1) {
            if (errno == EINTR) {
                if (parent_stop) break;
                continue;
            }
            perror("sem_wait items (parent)");
            break;
        }

        if (sem_wait(&shared->report_mutex) == -1) {
            perror("sem_wait report_mutex (parent)");
            sem_post(&shared->items); // попытка сохранить целостность
            break;
        }

        int idx = shared->reports_cons_idx % shared->buf_size;
        Report rep = shared->reports[idx]; // копируем наружу
        shared->reports_cons_idx++;
        shared->processed_reports++;

        sem_post(&shared->report_mutex);
        sem_post(&shared->slots);

        // Обработка отчёта
        char tbuf[64];
        struct tm tm;
        localtime_r(&rep.t, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

        sem_wait(&shared->print_mutex);
        cout << "[Silver] Получен отчёт: группа " << rep.group_id
             << " (pid=" << rep.group_pid << ") участок #" << rep.section
             << (rep.found ? " => Сундук НАЙДЕН!" : " => пусто")
             << "  time=" << tbuf << "\n";
        sem_post(&shared->print_mutex);
    }

    // Eсли прервано клавишей — оповещаем дочерние процессы
    if (parent_stop) {
        cout << "[Silver] Получен SIGINT, инициирую завершение дочерних процессов...\n";
        for (pid_t cpid : children) kill(cpid, SIGTERM);
    }

    // Ждём завершения всех дочерних процессов
    for (pid_t cpid : children) {
        int status;
        pid_t w = waitpid(cpid, &status, 0);
        if (w > 0) {
            cout << "[Silver] Дочерний pid=" << cpid << " завершился с кодом ";
            if (WIFEXITED(status)) {
                cout << WEXITSTATUS(status) << "\n";
            } 
            else if (WIFSIGNALED(status)) {
                cout << "signal " << WTERMSIG(status) << "\n";
            }
            else {
                cout << "unknown\n";
            }
        }
    }

    // Вывести краткий отчёт по проделанной работе
    sem_wait(&shared->print_mutex);
    cout << "[Silver] Обработано отчётов (декларировано): " << shared->processed_reports
         << " из " << total_to_process << "\n";
    sem_post(&shared->print_mutex);

    // Очистка: уничтожение семафоров и shared memory
    cleanup_shared_region(shm_name, shared, shm_size);
    cout << "[Silver] Семафоры и разделяемая память удалены. Программа завершена.\n";
    return 0;
}