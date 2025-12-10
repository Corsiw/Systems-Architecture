#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <vector>
#include <atomic>

#include <ostream>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>

using namespace std;

// Параметры генерации случайных чисел
constexpr int MIN_GROUP_DELAY = 200;
constexpr int MAX_GROUP_DELAY = 1500;

// Параметры запуска
int NUM_SECTIONS = 20; // Кол-во участков
int NUM_GROUPS = 5;    // Кол-во поисковых групп
double TREASURE_PROB = 0.05;
std::ofstream outFile;

// Глобальные данные
atomic<int> next_section(0);
int *section_taken = nullptr;

// Мьютексы
pthread_mutex_t mutex_alloc;
pthread_mutex_t mutex_reports;

// Условная переменная
pthread_cond_t cond_report;

// Структура доклада
struct Report {
  int group_id;
  int section_id;
  int found;
  int search_time;
};

// Массив докладов
Report *reports = nullptr;
atomic<int> reports_count(0);

// Вывод сообщений
void log_msg(const string &msg) {
  cout << msg << endl;
  if (outFile) {
    outFile << msg << "\n";
  }
}

// Обработка сигналов
static volatile sig_atomic_t g_terminate = 0;
void sigint_handler(int) { g_terminate = 1; }

// Поток группы
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
    section_id = next_section.fetch_add(1);
    section_taken[section_id] = 1;

    pthread_mutex_unlock(&mutex_alloc);

    log_msg("[Группа " + to_string(group_id) + "] Вышла на участок " +
            to_string(section_id));

    // Симуляция поиска
    int t = rand() % (MAX_GROUP_DELAY - MIN_GROUP_DELAY) + MIN_GROUP_DELAY;
    usleep(t * 1000);

    // Найдено сокровище?
    double r = (double)rand() / RAND_MAX;
    int found = (r < TREASURE_PROB);

    int idx = reports_count.fetch_add(1);
    reports[idx] = {group_id, section_id, found, t};

    // Сохраняем доклад
    pthread_mutex_lock(&mutex_reports);
    pthread_cond_signal(&cond_report);
    pthread_mutex_unlock(&mutex_reports);
  }

  log_msg("[Группа " + to_string(group_id) + "] завершила работу.");
  return nullptr;
}

// Поток Сильвера (главный)
void *silver_manager(void *) {
  int processed = 0;
  int found_total = 0;
  
  pthread_mutex_lock(&mutex_reports);
  while (processed < NUM_SECTIONS && !g_terminate) {
    // Ждём доклад
    pthread_cond_wait(&cond_report, &mutex_reports);

    // Берём один доклад
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

  return nullptr;
}

void read_args_from_file(char *&arg) {
  // Открываем файл, который содержит параметры
  ifstream inFile(arg);
  if (!inFile) {
    cerr << "Ошибка открытия файла " << arg << endl;
    exit(1);
  }

  vector<string> params;
  string token;
  while (inFile >> token) {
    params.push_back(token);
  }
  if (params.size() < 2 || params.size() > 3) {
    cerr << "Ошибка структуры входного файла: " << arg
         << " <кол-во групп> <кол-во участков> [файл для вывода]" << endl;
    exit(1);
  }
  NUM_GROUPS = stoi(params[0]);
  NUM_SECTIONS = stoi(params[1]);

  // Закрывай файл
  if (inFile) {
    inFile.close();
  }
  if (params.size() == 3) {
    outFile.open(params[2]);
    if (!outFile) {
      cerr << "Ошибка открытия файла для вывода " << params[2] << endl;
      exit(1);
    }
  }
}

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    string arg = argv[i];
    if (arg == "-g" || arg == "--groups") {
      NUM_GROUPS = stoi(argv[++i]);
    } else if (arg == "-s" || arg == "--sections") {
      NUM_SECTIONS = stoi(argv[++i]);
    } else if (arg == "-i" || arg == "--input-file") {
      read_args_from_file(argv[++i]);
      break;
    } else if (arg == "-o" || arg == "--output-file") {
      outFile.open(argv[++i]);
      if (!outFile) {
        cerr << "Не удалось открыть файл для вывода\n";
        return 1;
      }
    } else {
      cerr << "Неизвестный ключ: " << arg << endl;
      return 1;
    }
  }

  if (NUM_GROUPS <= 0 || NUM_SECTIONS <= 0 || NUM_GROUPS >= NUM_SECTIONS) {
    cerr << "Ошибка: число групп должно быть > 0 и < числа участков." << endl;
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
  pthread_cond_init(&cond_report, nullptr);

  // Создаём потоки групп
  pthread_t *threads = new pthread_t[NUM_GROUPS];
  for (int i = 0; i < NUM_GROUPS; i++) {
    pthread_create(&threads[i], nullptr, group_thread, (void *)(long)(i + 1));
  }

  // Управляющий поток (Сильвер)
  pthread_t silver_thread;
  pthread_create(&silver_thread, nullptr, silver_manager, nullptr);

  // Ждём завершения всех групп
  for (int i = 0; i < NUM_GROUPS; i++) {
    pthread_join(threads[i], nullptr);
  }

  // Ждём завершения Сильвера
  pthread_join(silver_thread, nullptr);

  // Очистка
  pthread_cond_destroy(&cond_report);
  pthread_mutex_destroy(&mutex_alloc);
  pthread_mutex_destroy(&mutex_reports);
  delete[] section_taken;
  delete[] reports;
  delete[] threads;

  if (outFile) {
    outFile.close();
  }

  return 0;
}
