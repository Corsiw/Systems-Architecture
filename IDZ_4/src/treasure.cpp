#include <cstdlib>
#include <ctime>
#include <iostream>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>

using namespace std;

// -------------------------
// Параметры запуска
// -------------------------
int NUM_SECTIONS = 20; // Кол-во участков
int NUM_GROUPS = 5;    // Кол-во поисковых групп
double TREASURE_PROB = 0.05;

// -------------------------
// Глобальные данные
// -------------------------
int next_section = 0;
int *section_taken = nullptr;

// Мьютексы
pthread_mutex_t mutex_alloc;
pthread_mutex_t mutex_reports;

// Семафор для докладов
sem_t sem_report;

// Структура доклада
struct Report {
  int group_id;
  int section_id;
  int found;
  int search_time;
};

// Массив докладов
Report *reports = nullptr;
int reports_count = 0;

// -------------------------
// Вывод сообщений
// -------------------------
void log_msg(const string &msg) { cout << msg << endl; }

static volatile sig_atomic_t g_terminate = 0;
void sigint_handler(int) { g_terminate = 1; }

// -------------------------
// Поток группы
// -------------------------
void *group_thread(void *arg) {
  int group_id = (int)(long)arg;

  int section_id = -1;
  while (!g_terminate) {
    // Выдаём участок
    pthread_mutex_lock(&mutex_alloc);
    if (next_section >= NUM_SECTIONS) {
      pthread_mutex_unlock(&mutex_alloc);
      break;
    }
    section_id = next_section;
    next_section++;
    section_taken[section_id] = 1;
    pthread_mutex_unlock(&mutex_alloc);

    log_msg("[Группа " + to_string(group_id) + "] Вышла на участок " +
            to_string(section_id));

    // Симуляция поиска
    int t = 200 + rand() % 1301; // 200..1500 ms
    usleep(t * 1000);

    // Найдено сокровище?
    double r = (double)rand() / RAND_MAX;
    int found = (r < TREASURE_PROB);

    // Сохраняем доклад
    pthread_mutex_lock(&mutex_reports);

    reports[reports_count].group_id = group_id;
    reports[reports_count].section_id = section_id;
    reports[reports_count].found = found;
    reports[reports_count].search_time = t;
    reports_count++;

    pthread_mutex_unlock(&mutex_reports);

    sem_post(&sem_report);
  }

  log_msg("[Группа " + to_string(group_id) + "] завершила работу.");
  return nullptr;
}

// -------------------------
// Поток Сильвера (главный)
// -------------------------
void silver_manager() {
  int processed = 0;
  int found_total = 0;

  while (processed < NUM_SECTIONS && !g_terminate) {
    // Ждём доклад
    sem_wait(&sem_report);

    // Берём один доклад
    pthread_mutex_lock(&mutex_reports);
    Report rep = reports[processed++];
    pthread_mutex_unlock(&mutex_reports);

    if (rep.found)
      found_total++;

    log_msg("[Сильвер] Доклад от группы " + to_string(rep.group_id) +
            ": участок " + to_string(rep.section_id) + ", время " +
            to_string(rep.search_time) + " ms, " +
            (rep.found ? "СОКРОВИЩЕ НАЙДЕНО!" : "пусто"));
  }

  log_msg("[Сильвер] Работа завершена. Найдено кладов: " +
          to_string(found_total));
}

// -------------------------
// MAIN
// -------------------------
int main(int argc, char *argv[]) {
  if (argc < 3) {
    cerr << "Использование: " << argv[0] << " <кол-во групп> <кол-во участков>"
         << endl;
    return 1;
  }

  NUM_GROUPS = atoi(argv[1]);
  NUM_SECTIONS = atoi(argv[2]);

  if (NUM_SECTIONS <= 0 || NUM_GROUPS <= 0 || NUM_GROUPS >= NUM_SECTIONS) {
    cerr << "Ошибка: число групп должно быть > 0 и < числа участков.";
    return 1;
  }

  signal(SIGTERM, sigint_handler);
  signal(SIGINT, sigint_handler);

  // Инициализация генератора случайных чисел
  srand(time(nullptr));

  // Подготовка данных
  section_taken = new int[NUM_SECTIONS]();
  reports = new Report[NUM_SECTIONS];
  reports_count = 0;

  pthread_mutex_init(&mutex_alloc, nullptr);
  pthread_mutex_init(&mutex_reports, nullptr);
  sem_init(&sem_report, 0, 0);

  // Создаём потоки групп
  pthread_t *threads = new pthread_t[NUM_GROUPS];
  for (int i = 0; i < NUM_GROUPS; i++) {
    pthread_create(&threads[i], nullptr, group_thread, (void *)(long)(i + 1));
  }

  // Управляющий поток (Сильвер)
  silver_manager();

  // Ждём завершения всех групп
  for (int i = 0; i < NUM_GROUPS; i++) {
    pthread_join(threads[i], nullptr);
  }

  // Очистка
  sem_destroy(&sem_report);
  pthread_mutex_destroy(&mutex_alloc);
  pthread_mutex_destroy(&mutex_reports);
  delete[] section_taken;
  delete[] reports;
  delete[] threads;

  return 0;
}
