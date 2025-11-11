#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

#include <fcntl.h>     // shm_open
#include <semaphore.h> // sem_t, sem_init...
#include <signal.h>
#include <sys/mman.h> // mmap, munmap
#include <sys/stat.h> // mode constants
#include <sys/wait.h> // waitpid
#include <unistd.h>   // fork, sleep, getpid

using namespace std;

static volatile sig_atomic_t g_stop = 0;
void sigint_handler(int) { g_stop = 1; }

constexpr size_t BUFFER_SIZE = 128;
constexpr size_t USLEEP_TIME = 300000;

struct Shared {
  int buffer[BUFFER_SIZE]; // Кольцевой буфер
  int g_stop;              // Остановка
  size_t buffer_size;      // Размер буфера
  size_t writeIndex;       // Куда писать
  size_t readIndex;        // Откуда читать

  sem_t empty; // Сколько свободных ячеек
  sem_t full;  // Сколько занятых ячеек
};

const string shm_name = "/random_number_client_server";
constexpr size_t shm_size = sizeof(Shared);

void cleanup_shared_region(const string &name, Shared *ptr, size_t size) {
  // Попытаться уничтожить семафоры (если ptr валиден)
  if (ptr) {
    if (sem_destroy(&ptr->empty) == -1)
      perror("sem_destroy empty");
    if (sem_destroy(&ptr->full) == -1)
      perror("sem_destroy full");
    munmap(ptr, size);
  }
  // Удаляем объект разделяемой памяти
  shm_unlink(name.c_str());
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout.setf(std::ios::unitbuf);
  ios::sync_with_stdio(false);
  cin.tie(nullptr);

  struct sigaction sa{};
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);

  shm_unlink(shm_name.c_str());

  // Создаём POSIX shared memory
  int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd == -1) {
    perror("shm_open");
    return 1;
  }

  if (ftruncate(fd, shm_size) == -1) {
    perror("ftruncate");
    shm_unlink(shm_name.c_str());
    return 1;
  }

  void *mem =
      mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    perror("mmap");
    shm_unlink(shm_name.c_str());
    return 1;
  }
  close(fd); // дескриптор можно закрыть, область останется доступной через mmap

  Shared *shared = static_cast<Shared *>(mem);

  // Инициализация управл. полей
  shared->g_stop = 0;
  shared->buffer_size = BUFFER_SIZE;
  shared->readIndex = 0;
  shared->writeIndex = 0;

  // Инициализируем неименованные POSIX семафоры в разделяемой памяти
  if (sem_init(&shared->empty, 1, BUFFER_SIZE) == -1 ||
      sem_init(&shared->full, 1, 0) == -1) {
    perror("sem_init");
    cleanup_shared_region(shm_name, shared, shm_size);
    return 1;
  }

  cout << "[Server pid=" << getpid() << "] запущен. Ожидается запуск Client\n";

  while (!g_stop && !shared->g_stop) {
    // ждём появления элемента
    sem_wait(&shared->full); // Ждём, пока есть данные

    int value = shared->buffer[shared->readIndex];
    shared->readIndex = (shared->readIndex + 1) % shared->buffer_size;

    sem_post(&shared->empty); // Освобождаем место

    std::cout << "Server read: " << value << "\n";
    usleep(USLEEP_TIME);
  }

  while (shared->readIndex < shared->writeIndex) {
    int value = shared->buffer[shared->readIndex];
    std::cout << "Server read: " << value << "\n";
    shared->readIndex = (shared->readIndex + 1) % shared->buffer_size;
  }

  // Eсли прервано клавишей — оповещаем Client процесс
  if (g_stop) {
    // выставляем флаг shutdown, чтобы все рабочие корректно завершились
    cout << "[Server] SIGINT получен — выставляю shutdown флаг и ожидаю "
            "завершения Client...\n";
    shared->g_stop = 1;
    sem_post(&shared->empty); // разблокировать клиента
    sem_post(&shared->full);  // разблокировать сервер
  }

  cleanup_shared_region(shm_name, shared, shm_size);
  cout << "[Server] Семафоры и разделяемая память удалены. Программа "
          "завершена.\n";
  return 0;
}