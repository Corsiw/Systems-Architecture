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
    // буфер фиксированного размера (будет использоваться как flexible array)
    // но здесь укажем один элемент; фактический размер учтём при mmap.
    Report reports[1];
};

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
    ios::sync_with_stdio(false);
    cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);

    if(argc < 2 || string(argv[1]) != "open"){
        cerr << "Usage: " << argv[0] << " open\n";
        return 1;
    }

    signal(SIGTERM, sigint_handler);
    signal(SIGINT, sigint_handler);

    string s_shm = get_shm_name();
    int fd = shm_open(s_shm.c_str(), O_RDWR, 0);
    if(fd == -1){
        perror("shm_open (worker) - убедитесь, что менеджер запущен");
        return 1;
    }
    // нужно получить размер файла чтобы mmap
    struct stat st;
    if(fstat(fd, &st) == -1){
        perror("fstat");
        close(fd);
        return 1;
    }
    size_t shm_sz = st.st_size;
    void* mem = mmap(nullptr, shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(mem == MAP_FAILED){
        perror("mmap");
        close(fd);
        return 1;
    }
    close(fd);

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

    if(section_mutex == SEM_FAILED || report_mutex == SEM_FAILED || items_mutex == SEM_FAILED ||
       slots_mutex == SEM_FAILED || workers_mutex == SEM_FAILED){
        perror("sem_open (worker)");
        return 1;
    }

    // Проверка лимита
    sem_wait(workers_mutex);
    if (shared->active_workers >= shared->max_workers) {
        std::cerr << "[Worker " << getpid() << "] Максимальное число активных групп ("
                << shared->max_workers << ") уже достигнуто. Завершение.\n";
        sem_post(workers_mutex);
        return 0;
    }
    shared->active_workers++;
    sem_post(workers_mutex);

    // Простая генерация id группы на основе PID
    int group_id = (int)(getpid() % 10000);
    srand((unsigned)time(nullptr) ^ getpid());

    while(!g_terminate) {
        if(shared->shutdown) {
            cout << "[Worker pid=" << getpid() << "] замечен shutdown флаг — завершаюсь.\n";
            break;
        }

        if(sem_wait(section_mutex) == -1) {
            if(errno == EINTR) continue;
            perror("sem_wait mutex (worker)");
            break;
        }

        int section = shared->next_section;
        if(section >= shared->total_sections) {
            // больше участков
            sem_post(section_mutex);
            cout << "[Worker pid=" << getpid() << "] участков больше нет — завершаюсь.\n";
            break;
        }
        shared->next_section++;
        sem_post(section_mutex);

        int work = 1 + rand() % 3;
        cout << "[Worker pid=" << getpid() << "] берёт участок #" << section << ", время " << work << "s\n";
        sleep(work);
        bool found = (rand() % 100) < 10;

        // положить отчёт в буфер
        if(sem_wait(slots_mutex) == -1){
            if(errno == EINTR) continue;
            perror("sem_wait slots (worker)");
            break;
        }
        if(sem_wait(report_mutex) == -1){
            perror("sem_wait report_mutex (worker)");
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

        cout << "[Worker pid=" << getpid() << "] отправил отчёт по участку #" << section
             << (found ? " (НАШЁЛ!)" : " (ничего)") << "\n";

        // небольшая пауза
        sleep(rand() % 2);
    }

    sem_close(section_mutex);
    sem_close(report_mutex);
    sem_close(items_mutex);
    sem_close(slots_mutex);

    munmap(mem, shm_sz);

    cout << "[Worker pid=" << getpid() << "] завершился корректно.\n";
    return 0;
}