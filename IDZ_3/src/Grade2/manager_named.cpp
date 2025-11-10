#include <iostream>
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
    // буфер фиксированного размера (будет использоваться как flexible array)
    // но здесь укажем один элемент; фактический размер учтём при mmap.
    Report reports[1];
};

size_t shmsize_for(int buf_size) {
    return sizeof(Shared) + (size_t)(buf_size - 1) * sizeof(Report);
}

string base_name = "/treasure_demo";

string get_shm_name() {
    return base_name + "_shm";
}

string get_shm_name(const string &sfx) { 
    return base_name + sfx;
}

// helper, чтобы анлинкнуть старые семафоры
void safe_sem_unlink(const char* name){
    if(sem_unlink(name) == -1){
        // ignore
    }
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

    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    string shm_name = get_shm_name();
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
    shared->shutdown = 0;

    // named semaphores
    string s_mutex = get_shm_name("_mutex");
    string s_report = get_shm_name("_report");
    string s_items = get_shm_name("_items");
    string s_slots = get_shm_name("_slots");

    // Удаляем существующие semaphores, если создались с крашем
    safe_sem_unlink(s_mutex.c_str());
    safe_sem_unlink(s_report.c_str());
    safe_sem_unlink(s_items.c_str());
    safe_sem_unlink(s_slots.c_str());

    // Инициализируем семафоры
    sem_t* section_mutex = sem_open(s_mutex.c_str(), O_CREAT | O_EXCL, 0600, 1);
    sem_t* report_mutex = sem_open(s_report.c_str(), O_CREAT | O_EXCL, 0600, 1);
    sem_t* items_mutex = sem_open(s_items.c_str(), O_CREAT | O_EXCL, 0600, 0);
    sem_t* slots_mutex = sem_open(s_slots.c_str(), O_CREAT | O_EXCL, 0600, buf_size);

    if(section_mutex == SEM_FAILED || report_mutex == SEM_FAILED || items_mutex == SEM_FAILED ||
       slots_mutex == SEM_FAILED){
        perror("sem_open (create)");

        if(section_mutex != SEM_FAILED) sem_close(section_mutex);
        if(report_mutex != SEM_FAILED) sem_close(report_mutex);
        if(items_mutex != SEM_FAILED) sem_close(items_mutex);
        if(slots_mutex != SEM_FAILED) sem_close(slots_mutex);
        shm_unlink(shm_name.c_str());
        return 1;
    }

    cout << "Manager(pid=" << getpid() << "): создана SHM и семафоры.\n";
    cout << "SHM name: " << shm_name << "\n";
    cout << "Semaphores: " << s_mutex << ", " << s_report << ", " << s_items << ", " << s_slots << ", " << "\n";
    cout << "Ожидайте запуска рабочих в других консолях командой: ./worker_named open\n";

    // Сильвер — принимает отчёты
    int total_to_process = num_sections;
    while(!g_stop && shared->processed_reports < total_to_process){
        // ждём появления элемента
        if (sem_wait(items_mutex) == -1) {
            if (errno == EINTR) {
                if (g_stop) break;
                continue;
            }
            perror("sem_wait items");
            break;
        }

        if (sem_wait(report_mutex) == -1) {
            perror("sem_wait report_mutex");
            sem_post(items_mutex); // попытка сохранить целостность
            break;
        }

        int idx = shared->reports_cons_idx % shared->buf_size;
        Report rep = shared->reports[idx]; // копируем наружу
        shared->reports_cons_idx++;
        shared->processed_reports++;

        sem_post(report_mutex);
        sem_post(slots_mutex);

        // Обработка отчёта
        char tbuf[64];
        struct tm tm;
        localtime_r(&rep.t, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        cout << "[Silver] Получен отчёт: группа " << rep.group_id
             << " (pid=" << rep.group_pid << ") участок #" << rep.section
             << (rep.found ? " => Сундук НАЙДЕН!" : " => пусто")
             << "  time=" << tbuf << "\n";
    }

    // Eсли прервано клавишей — оповещаем worker процессы
    if (g_stop) {
        // выставляем флаг shutdown, чтобы все рабочие корректно завершились
        cout << "Manager: SIGINT получен — выставляю shutdown флаг и ожидаю завершения рабочих...\n";
        shared->shutdown = 1;
    }

    sleep(2);

    cout << "Manager: обработано отчётов: " << shared->processed_reports << " из " << total_to_process << "\n";

    // Очистка: уничтожение семафоров и shared memory
    sem_close(section_mutex);
    sem_close(report_mutex);
    sem_close(items_mutex);
    sem_close(slots_mutex);

    // unlink именованных семафоров
    safe_sem_unlink(s_mutex.c_str());
    safe_sem_unlink(s_report.c_str());
    safe_sem_unlink(s_items.c_str());
    safe_sem_unlink(s_slots.c_str());

    munmap(mem, shm_size);
    shm_unlink(shm_name.c_str());

    cout << "[Silver] Семафоры и разделяемая память удалены. Программа завершена.\n";
    return 0;
}