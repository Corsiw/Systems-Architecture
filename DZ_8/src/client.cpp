#include <cstddef>
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

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout.setf(std::ios::unitbuf);
  ios::sync_with_stdio(false);
  cin.tie(nullptr);

  srand(time(nullptr));

  // Обработчик Ctrl + C
  struct sigaction sa{};
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);

  // Создаём POSIX shared memory
  int fd = shm_open(shm_name.c_str(), O_RDWR, 0);
  if (fd == -1) {
    perror("shm_open. Возможно Server не запущен");
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

  cout << "[Client pid=" << getpid() << "] запущен. Ожидается запуск Server\n";

  while (!g_stop && !shared->g_stop) {
    int number = rand() % 1000;

    sem_wait(&shared->empty); // Ждём свободное место

    shared->buffer[shared->writeIndex] = number;
    shared->writeIndex = (shared->writeIndex + 1) % shared->buffer_size;

    sem_post(&shared->full); // Сообщаем, что есть данные

    std::cout << "Client wrote: " << number << "\n";
    usleep(USLEEP_TIME);
  }

  // Eсли прервано клавишей — оповещаем Server процесс
  if (g_stop) {
    // выставляем флаг shutdown, чтобы Server корректно завершилися
    cout << "[Client] SIGINT получен — выставляю shutdown флаг и завершаю "
            "работу\n";
    shared->g_stop = 1;
    sem_post(&shared->empty); // разблокировать клиента
    sem_post(&shared->full);  // разблокировать сервер
  }

  // Размэппим память (не удаляем сегмент — это делает сервер)
  if (munmap(shared, shm_size) == -1) {
    perror("munmap");
  }
  
  return 0;
}