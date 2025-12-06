#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <unistd.h>

constexpr size_t SOURCE_COUNT = 100;
constexpr size_t MIN_SOURCE_SLEEP = 1;
constexpr size_t MAX_SOURCE_SLEEP = 7;
constexpr size_t MIN_SUM_SLEEP = 3;
constexpr size_t MAX_SUM_SLEEP = 6;
constexpr size_t BUFFER_SIZE =
    2 * SOURCE_COUNT; // Максимально возможное число элементов

int buffer[BUFFER_SIZE];
size_t read_index = 0;
size_t write_index = 0;

pthread_mutex_t buffer_mutex;
pthread_cond_t buffer_cond;

bool all_sources_done = false;
int active_sums = 0;

unsigned int seed = 69;

void *source_thread_function(void *) {
  int sleep_time =
      rand() % (MAX_SOURCE_SLEEP - MIN_SOURCE_SLEEP + 1) + MIN_SOURCE_SLEEP;
  sleep(sleep_time);

  int num = rand() % 100 + 1;

  pthread_mutex_lock(&buffer_mutex);
  if (write_index < BUFFER_SIZE) { // защита от переполнения
    buffer[write_index++] = num;
    std::cout << "Source writes " << num
              << " to buffer. Current size: " << write_index - read_index
              << "\n";
  }
  pthread_cond_signal(&buffer_cond);
  pthread_mutex_unlock(&buffer_mutex);

  return nullptr;
}

struct SumArg {
  int a, b;
};

void *sum_thread_function(void *arg) {
  SumArg *sarg = (SumArg *)arg;

  int sleep_time = rand() % (MAX_SUM_SLEEP - MIN_SUM_SLEEP + 1) + MIN_SUM_SLEEP;
  sleep(sleep_time);

  int sum = sarg->a + sarg->b;
  delete sarg;

  pthread_mutex_lock(&buffer_mutex);
  if (write_index < BUFFER_SIZE) {
    buffer[write_index++] = sum;
    active_sums--;
    std::cout << "Sum writes " << sum
              << " to buffer. Current size: " << write_index - read_index
              << "\n";

    pthread_cond_signal(&buffer_cond);
  }
  pthread_mutex_unlock(&buffer_mutex);

  return nullptr;
}

void *monitor_thread_function(void *) {
  while (true) {
    pthread_mutex_lock(&buffer_mutex);
    // Ждем хотя бы 2 числа или пока есть активные сумматоры/источники
    while ((write_index - read_index < 2) &&
           !(all_sources_done && active_sums == 0)) {
      pthread_cond_wait(&buffer_cond, &buffer_mutex);
    }

    // Условие завершения: один элемент и нет активных сумматоров
    if (write_index - read_index == 1 && all_sources_done && active_sums == 0) {
      pthread_mutex_unlock(&buffer_mutex);
      break;
    }

    // Суммируем пары чисел
    while (write_index - read_index >= 2) {
      int a = buffer[read_index++];
      int b = buffer[read_index++];

      active_sums++;
      pthread_mutex_unlock(&buffer_mutex);

      SumArg *sarg = new SumArg{a, b};
      pthread_t tid;
      pthread_create(&tid, nullptr, sum_thread_function, sarg);
      pthread_detach(tid);

      pthread_mutex_lock(&buffer_mutex);
    }

    pthread_mutex_unlock(&buffer_mutex);
  }
  return nullptr;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  srand(seed);

  pthread_mutex_init(&buffer_mutex, nullptr);
  pthread_cond_init(&buffer_cond, nullptr);

  std::cout << "Начало работы...\n";

  pthread_t source_threads[SOURCE_COUNT];
  for (int i = 0; i < SOURCE_COUNT; ++i)
    pthread_create(&source_threads[i], nullptr, source_thread_function,
                   nullptr);

  pthread_t monitor;
  pthread_create(&monitor, nullptr, monitor_thread_function, nullptr);

  // Ждем источники
  for (int i = 0; i < SOURCE_COUNT; ++i)
    pthread_join(source_threads[i], nullptr);

  // Сообщаем монитору, что источники завершены
  pthread_mutex_lock(&buffer_mutex);
  all_sources_done = true;
  pthread_cond_signal(&buffer_cond);
  pthread_mutex_unlock(&buffer_mutex);

  // Ждем монитор
  pthread_join(monitor, nullptr);

  // Финальная сумма
  pthread_mutex_lock(&buffer_mutex);
  if (write_index - read_index == 1)
    std::cout << "Итоговая сумма: " << buffer[read_index] << "\n";
  pthread_mutex_unlock(&buffer_mutex);

  pthread_mutex_destroy(&buffer_mutex);
  pthread_cond_destroy(&buffer_cond);

  return 0;
}
